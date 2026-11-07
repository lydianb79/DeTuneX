import DSPCore
import MetalKit
import simd
import SwiftUI

/// Realtime oscilloscope rendered with Metal: dry input (dim) and shifted
/// output (bright, glowing) traces, trigger-stabilised on the output.
struct MetalScopeView: NSViewRepresentable {
    let processor: OpaquePointer
    var theme: Theme = .stealth

    func makeCoordinator() -> ScopeRenderer { ScopeRenderer(processor: processor) }

    func makeNSView(context: Context) -> MTKView {
        let view = MTKView()
        view.device = context.coordinator.device
        view.colorPixelFormat = .bgra8Unorm
        apply(theme, to: view, renderer: context.coordinator)
        view.preferredFramesPerSecond = 60
        view.framebufferOnly = true
        view.enableSetNeedsDisplay = false
        view.isPaused = false
        view.delegate = context.coordinator
        view.layer?.isOpaque = true
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {
        apply(theme, to: view, renderer: context.coordinator)
    }

    private func apply(_ t: Theme, to view: MTKView, renderer: ScopeRenderer) {
        view.clearColor = MTLClearColor(red: Double(t.scopeBase.x), green: Double(t.scopeBase.y), blue: Double(t.scopeBase.z), alpha: 1)
        renderer.accent = t.accentRGBA
        renderer.dryColor = SIMD4(t.traceDry, 0.55)
        renderer.base = SIMD4(t.scopeBase, 1)
        renderer.glowColor = SIMD4(t.scopeGlow, 1)
    }
}

// Must match `TraceUniforms` in the shader (float4, float2, float, float, float, uint).
private struct TraceUniforms {
    var color: SIMD4<Float>
    var viewport: SIMD2<Float>
    var thickness: Float
    var gain: Float
    var yCenter: Float
    var count: UInt32
}

// Must match `GridUniforms` in the shader (float4, float4, float2, float, float).
private struct GridUniforms {
    var base: SIMD4<Float>
    var glowColor: SIMD4<Float>
    var viewport: SIMD2<Float>
    var time: Float
    var glow: Float
}

final class ScopeRenderer: NSObject, MTKViewDelegate {
    let device: MTLDevice?
    private let queue: MTLCommandQueue?
    private var tracePipeline: MTLRenderPipelineState?
    private var gridPipeline: MTLRenderPipelineState?
    private let processor: OpaquePointer
    var accent = SIMD4<Float>(0.33, 0.78, 1.0, 1.0)
    var dryColor = SIMD4<Float>(0.55, 0.58, 0.66, 0.55)
    var base = SIMD4<Float>(0.035, 0.04, 0.055, 1)
    var glowColor = SIMD4<Float>(0.10, 0.30, 0.45, 1)

    private static let readCount = 2048
    private static let displayCount = 1024
    private var dry = [Float](repeating: 0, count: readCount)
    private var wet = [Float](repeating: 0, count: readCount)
    private var dryBuffer: MTLBuffer?
    private var wetBuffer: MTLBuffer?
    private var start = CACurrentMediaTime()
    private var smoothedGain: Float = 1

    init(processor: OpaquePointer) {
        self.processor = processor
        device = MTLCreateSystemDefaultDevice()
        queue = device?.makeCommandQueue()
        super.init()
        buildPipelines()
        dryBuffer = device?.makeBuffer(length: Self.displayCount * 4, options: .storageModeShared)
        wetBuffer = device?.makeBuffer(length: Self.displayCount * 4, options: .storageModeShared)
    }

    private func buildPipelines() {
        guard let device else { return }
        do {
            // Compiled at runtime so the package builds with plain `swift build`.
            let library = try device.makeLibrary(source: Self.shaderSource, options: nil)
            func pipeline(_ v: String, _ f: String, additive: Bool) throws -> MTLRenderPipelineState {
                let d = MTLRenderPipelineDescriptor()
                d.vertexFunction = library.makeFunction(name: v)
                d.fragmentFunction = library.makeFunction(name: f)
                let c = d.colorAttachments[0]!
                c.pixelFormat = .bgra8Unorm
                c.isBlendingEnabled = true
                c.rgbBlendOperation = .add
                c.alphaBlendOperation = .add
                c.sourceRGBBlendFactor = .sourceAlpha
                c.destinationRGBBlendFactor = additive ? .one : .oneMinusSourceAlpha
                c.sourceAlphaBlendFactor = .one
                c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
                return try device.makeRenderPipelineState(descriptor: d)
            }
            tracePipeline = try pipeline("traceVertex", "traceFragment", additive: true)
            gridPipeline = try pipeline("gridVertex", "gridFragment", additive: false)
        } catch {
            NSLog("De-TuneX: Metal pipeline error: \(error)")
        }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

    func draw(in view: MTKView) {
        guard let queue, let tracePipeline, let gridPipeline,
              let pass = view.currentRenderPassDescriptor, let drawable = view.currentDrawable,
              let dryBuffer, let wetBuffer,
              let cmd = queue.makeCommandBuffer(),
              let enc = cmd.makeRenderCommandEncoder(descriptor: pass) else { return }

        // Pull the latest audio and find a rising zero crossing on the output
        // so periodic waveforms stand still.
        dtx_read_scope(processor, &dry, &wet, Int32(Self.readCount))
        let n = Self.displayCount
        var trigger = Self.readCount - n
        var i = Self.readCount - n
        while i > 1 {
            if wet[i - 1] < 0 && wet[i] >= 0 { trigger = i; break }
            i -= 1
        }
        var peak: Float = 0.02
        for k in 0..<n { peak = max(peak, abs(wet[trigger + k]), abs(dry[trigger + k])) }
        // Auto-gain so quiet guitar signals are visible; smooth to avoid pumping.
        let target = min(0.85 / peak, 24)
        smoothedGain += (target - smoothedGain) * (target < smoothedGain ? 0.5 : 0.05)

        dry.withUnsafeBufferPointer { src in
            dryBuffer.contents().copyMemory(from: src.baseAddress! + trigger, byteCount: n * 4)
        }
        wet.withUnsafeBufferPointer { src in
            wetBuffer.contents().copyMemory(from: src.baseAddress! + trigger, byteCount: n * 4)
        }

        let size = SIMD2<Float>(Float(view.drawableSize.width), Float(view.drawableSize.height))
        let scale = Float(view.window?.backingScaleFactor ?? 2)
        let wetAmount = dtx_get_wet_amount(processor)

        // Background grid with a soft glow that follows the effect engagement.
        var grid = GridUniforms(base: base, glowColor: glowColor, viewport: size,
                                time: Float(CACurrentMediaTime() - start), glow: wetAmount)
        enc.setRenderPipelineState(gridPipeline)
        enc.setFragmentBytes(&grid, length: MemoryLayout<GridUniforms>.stride, index: 0)
        enc.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)

        enc.setRenderPipelineState(tracePipeline)
        func trace(_ buffer: MTLBuffer, color: SIMD4<Float>, thickness: Float, yCenter: Float) {
            var u = TraceUniforms(color: color, viewport: size, thickness: thickness * scale,
                                  gain: smoothedGain * 0.42, yCenter: yCenter, count: UInt32(n))
            enc.setVertexBuffer(buffer, offset: 0, index: 0)
            enc.setVertexBytes(&u, length: MemoryLayout<TraceUniforms>.stride, index: 1)
            enc.setFragmentBytes(&u, length: MemoryLayout<TraceUniforms>.stride, index: 1)
            enc.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: n * 2)
        }
        var glow = accent; glow.w = 0.10 + 0.12 * wetAmount
        // Dry input (upper half), output with a wide glow pass then a sharp core (lower half).
        trace(dryBuffer, color: dryColor, thickness: 1.6, yCenter: 0.5)
        trace(wetBuffer, color: glow, thickness: 9, yCenter: -0.4)
        trace(wetBuffer, color: accent, thickness: 2.2, yCenter: -0.4)

        enc.endEncoding()
        cmd.present(drawable)
        cmd.commit()
    }

    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct TraceUniforms {
        float4 color;
        float2 viewport;
        float  thickness;
        float  gain;
        float  yCenter;
        uint   count;
    };

    struct TraceOut {
        float4 position [[position]];
        float  edge;
    };

    static float2 tracePoint(constant float* s, uint i, constant TraceUniforms& u) {
        float x = (float(i) / float(u.count - 1)) * u.viewport.x;
        float y = (0.5 - 0.5 * (u.yCenter + clamp(s[i] * u.gain, -0.48, 0.48))) * u.viewport.y;
        return float2(x, y);
    }

    vertex TraceOut traceVertex(uint vid [[vertex_id]],
                                constant float* samples [[buffer(0)]],
                                constant TraceUniforms& u [[buffer(1)]]) {
        uint i = vid / 2;
        float side = (vid & 1) ? 1.0 : -1.0;
        uint i0 = i > 0 ? i - 1 : 0;
        uint i1 = min(i + 1, u.count - 1);
        float2 p = tracePoint(samples, i, u);
        float2 d = tracePoint(samples, i1, u) - tracePoint(samples, i0, u);
        float len = max(length(d), 1e-4);
        float2 normal = float2(-d.y, d.x) / len;
        float2 q = p + normal * side * u.thickness * 0.5;
        TraceOut o;
        o.position = float4(q.x / u.viewport.x * 2.0 - 1.0, 1.0 - q.y / u.viewport.y * 2.0, 0, 1);
        o.edge = side;
        return o;
    }

    fragment float4 traceFragment(TraceOut in [[stage_in]],
                                  constant TraceUniforms& u [[buffer(1)]]) {
        float a = 1.0 - smoothstep(0.35, 1.0, abs(in.edge));
        return float4(u.color.rgb, u.color.a * a);
    }

    struct GridUniforms {
        float4 base;
        float4 glowColor;
        float2 viewport;
        float  time;
        float  glow;
    };

    struct GridOut { float4 position [[position]]; float2 uv; };

    vertex GridOut gridVertex(uint vid [[vertex_id]]) {
        float2 pos = float2(float((vid << 1) & 2u), float(vid & 2u));
        GridOut o;
        o.position = float4(pos * 2.0 - 1.0, 0, 1);
        o.uv = float2(pos.x, 1.0 - pos.y);
        return o;
    }

    fragment float4 gridFragment(GridOut in [[stage_in]], constant GridUniforms& g [[buffer(0)]]) {
        float2 px = in.uv * g.viewport;
        float cell = g.viewport.y / 8.0;
        float2 f = abs(fract(px / cell + 0.5) - 0.5) * cell;
        float line = 1.0 - smoothstep(0.0, 1.2, min(f.x, f.y));
        float mid = 1.0 - smoothstep(0.0, 1.5, abs(px.y - g.viewport.y * 0.5));
        float3 col = g.base.rgb + line * 0.035 + mid * 0.05;
        // Engagement glow from the bottom edge.
        float v = 1.0 - in.uv.y;
        float pulse = 0.85 + 0.15 * sin(g.time * 2.0);
        col += g.glowColor.rgb * g.glow * pulse * pow(max(1.0 - v, 0.0), 3.0) * 0.6;
        return float4(col, 1.0);
    }
    """
}
