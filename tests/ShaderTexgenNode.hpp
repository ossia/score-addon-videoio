#pragma once

/**
 * @file ShaderTexgenNode.hpp
 * @brief GPU-generated test pattern: the AJARoundtrip gradient + rolling
 *        index band, computed in the fragment shader.
 *
 * Replaces TexgenNode+g_paint for the roundtrip harnesses: the CPU path
 * paints and uploads the full RGBA frame every tick (132 MB/frame at 8K),
 * which dominates producer time at ≥4K. Here the pattern is a pure
 * function of (pixel, frameIndex), so the producer cost is a fragment
 * pass — resolution-independent on the CPU side.
 *
 * Pattern (bit-exact with AJARoundtrip's CPU paint(), integer math):
 *   band  = max(1, h/8) top rows: R/G/B = 32 + 64*(2-bit field of idx)
 *   below: R = (x*255)/w, G = (y*255)/h, B = 128, A = 255
 *
 * The 6-bit index arrives through the standard process UBO's frameIndex
 * (folded to [0,64) by the renderer). `onFrame` fires on the render
 * thread right when the index is latched — the harness stamps send-time
 * there, replacing g_paint's stamp.
 *
 * Structure (bindings, mesh, material init) intentionally mirrors
 * score::gfx::TexgenNode so the pipeline setup is known-good on every
 * backend; the 1x1 dummy texture keeps the sampler binding layout
 * identical at negligible cost.
 */

#include <Gfx/Graph/Node.hpp>
#include <Gfx/Graph/NodeRenderer.hpp>
#include <Gfx/Graph/RenderList.hpp>
#include <Gfx/Graph/RenderState.hpp>
#include <Gfx/Graph/Uniforms.hpp>

#include <functional>

namespace score::gfx
{

struct ShaderTexgenNode : NodeModel
{
  static const constexpr auto vertex = R"_(#version 450
  layout(location = 0) in vec2 position;
  layout(location = 1) in vec2 texcoord;

  layout(binding = 3) uniform sampler2D y_tex;
  layout(location = 0) out vec2 v_texcoord;

  layout(std140, binding = 0) uniform renderer_t {
    mat4 clipSpaceCorrMatrix;
    vec2 renderSize;
  } renderer;

  out gl_PerVertex { vec4 gl_Position; };

  void main()
  {
    v_texcoord = texcoord;
    gl_Position = renderer.clipSpaceCorrMatrix * vec4(position.xy, 0.0, 1.);
#if !(defined(QSHADER_SPIRV) || defined(QSHADER_HLSL) || defined(QSHADER_MSL))
  gl_Position.y = - gl_Position.y;
#endif
  }
  )_";

  static const constexpr auto filter = R"_(#version 450
  layout(location = 0) in vec2 v_texcoord;
  layout(location = 0) out vec4 fragColor;

  layout(std140, binding = 0) uniform renderer_t {
  mat4 clipSpaceCorrMatrix;
  vec2 renderSize;
  } renderer;

  layout(std140, binding = 1) uniform process_t {
    float time;
    float timeDelta;
    float progress;
    float sampleRate;
    int passIndex;
    int frameIndex;
    vec2 renderSize;
    vec4 date;
  } process;

  layout(binding=3) uniform sampler2D y_tex;

  void main ()
  {
    int w = int(renderer.renderSize.x);
    int h = int(renderer.renderSize.y);
    ivec2 p = clamp(
        ivec2(v_texcoord * renderer.renderSize), ivec2(0), ivec2(w - 1, h - 1));
    int band = max(1, h / 8);
    int idx = process.frameIndex;

    ivec3 v;
    if(p.y < band)
    {
      v = ivec3(
          32 + 64 * (idx & 3), 32 + 64 * ((idx >> 2) & 3),
          32 + 64 * ((idx >> 4) & 3));
    }
    else
    {
      v = ivec3((p.x * 255) / w, (p.y * 255) / h, 128);
    }
    fragColor = vec4(vec3(v) / 255.0, 1.0);
  }
  )_";

  struct Rendered : GenericNodeRenderer
  {
    using GenericNodeRenderer::GenericNodeRenderer;

    ~Rendered() { }

    QRhiTexture* texture{};

    void initState(RenderList& renderer, QRhiResourceUpdateBatch& res) override
    {
      m_mesh = &renderer.defaultTriangle();
      defaultMeshInit(renderer, *m_mesh, res);
      processUBOInit(renderer);
      m_material.init(renderer, node.input, m_samplers);
      std::tie(m_vertexS, m_fragmentS)
          = score::gfx::makeShaders(renderer.state, vertex, filter);

      auto& rhi = *renderer.state.rhi;
      {
        texture = rhi.newTexture(
            QRhiTexture::RGBA8, QSize(1, 1), 1, QRhiTexture::Flag{});
        texture->create();
        static const unsigned char white[4] = {255, 255, 255, 255};
        QRhiTextureSubresourceUploadDescription subdesc{
            QByteArray::fromRawData((const char*)white, 4)};
        res.uploadTexture(texture, QRhiTextureUploadDescription{{0, 0, subdesc}});
      }

      {
        auto sampler = rhi.newSampler(
            QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
            QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge);
        sampler->create();
        m_samplers.push_back({sampler, texture});
      }

      m_initialized = true;
    }

    void update(
        RenderList& renderer, QRhiResourceUpdateBatch& res,
        score::gfx::Edge* edge) override
    {
      if(!edge)
        return;

      auto& n = const_cast<ShaderTexgenNode&>(
          static_cast<const ShaderTexgenNode&>(this->node));
      const int idx = t % 64;
      n.standardUBO.frameIndex = idx;
      if(n.onFrame)
        n.onFrame(idx);
      ++t;

      defaultUBOUpdate(renderer, res);
    }

    void releaseState(RenderList& r) override
    {
      if(texture)
      {
        texture->deleteLater();
        texture = nullptr;
      }
      GenericNodeRenderer::releaseState(r);
    }

    int t = 0;
  };

  ShaderTexgenNode() { output.push_back(new Port{this, {}, Types::Image, {}}); }
  ~ShaderTexgenNode() override { m_materialData.release(); }

  /// Called on the render thread with the folded index, at the moment the
  /// frame's UBO is latched — the harness stamps send-time here.
  std::function<void(int)> onFrame;

  score::gfx::NodeRenderer* createRenderer(RenderList& r) const noexcept override
  {
    return new Rendered{*this};
  }
};

} // namespace score::gfx
