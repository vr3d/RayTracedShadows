#include "RayTracedShadows.h"
#include <Rush/UtilFile.h>
#include <Rush/UtilLog.h>
#include <Rush/MathTypes.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize.h>

#include <tiny_obj_loader.h>

AppConfig g_appConfig;

int main(int argc, char** argv)
{
	g_appConfig.name = "RayTracedShadows (" RUSH_RENDER_API_NAME ")";

	g_appConfig.width = 1280;
	g_appConfig.height = 720;
	g_appConfig.argc = argc;
	g_appConfig.argv = argv;
	g_appConfig.resizable = true;

#ifndef NDEBUG
	g_appConfig.debug = true;
#endif

	return Platform_Main<RayTracedShadowsApp>(g_appConfig);
}

struct TimingScope
{
	TimingScope(MovingAverage<double, 60>& output)
		: m_output(output)
	{}

	~TimingScope()
	{
		m_output.add(m_timer.time());
	}

	MovingAverage<double, 60>& m_output;
	Timer m_timer;
};

RayTracedShadowsApp::RayTracedShadowsApp()
	: m_boundingBox(Vec3(0.0f), Vec3(0.0f))
{
	Gfx_SetPresentInterval(1);

	m_windowEvents.setOwner(m_window);

	const u32 whiteTexturePixels[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	GfxTextureDesc textureDesc = GfxTextureDesc::make2D(2, 2);
	m_defaultWhiteTexture = Gfx_CreateTexture(textureDesc, whiteTexturePixels);

	createRenderTargets(m_window->getSize());

	const char* shaderDirectory = Platform_GetExecutableDirectory();
	auto shaderFromFile = [&](const char* filename, GfxShaderType type) {
		std::string fullFilename = std::string(shaderDirectory) + "/" + std::string(filename);
		Log::message("Loading shader '%s'", filename);

		GfxShaderSource source;
		source.type = GfxShaderSourceType_SPV;

		FileIn f(fullFilename.c_str());
		if (f.valid())
		{
			u32 fileSize = f.length();
			source.resize(fileSize);
			f.read(&source[0], fileSize);
		}

		if (source.empty())
		{
			Log::error("Failed to load shader '%s'", filename);
		}

		return source;
	};

	{
		GfxVertexShaderRef modelVS;
		modelVS.takeover(Gfx_CreateVertexShader(shaderFromFile("Shaders/Model.vert.spv", GfxShaderType::Vertex)));

		GfxPixelShaderRef modelPS;
		modelPS.takeover(Gfx_CreatePixelShader(shaderFromFile("Shaders/Model.frag.spv", GfxShaderType::Pixel)));

		GfxVertexFormatDesc modelVFDesc;
		modelVFDesc.add(0, GfxVertexFormatDesc::DataType::Float3, GfxVertexFormatDesc::Semantic::Position, 0);
		modelVFDesc.add(0, GfxVertexFormatDesc::DataType::Float3, GfxVertexFormatDesc::Semantic::Normal, 0);
		modelVFDesc.add(0, GfxVertexFormatDesc::DataType::Float2, GfxVertexFormatDesc::Semantic::Texcoord, 0);

		GfxVertexFormatRef modelVF;
		modelVF.takeover(Gfx_CreateVertexFormat(modelVFDesc));

		GfxShaderBindings bindings;
		bindings.addConstantBuffer("Global", 0);
		bindings.addConstantBuffer("Material", 1);
		bindings.addSampler("albedoSampler", 0);
		m_techniqueModel = Gfx_CreateTechnique(GfxTechniqueDesc(modelPS.get(), modelVS.get(), modelVF.get(), &bindings));
	}

	{
		GfxComputeShaderRef cs;
		cs.takeover(Gfx_CreateComputeShader(shaderFromFile("Shaders/RayTracedShadows.comp.spv", GfxShaderType::Compute)));

		GfxShaderBindings bindings;

		bindings.addConstantBuffer("Constants", 0);
		bindings.addSampler("gbufferPositionSampler", 0);
		bindings.addStorageImage("outputShadowMask", 0);
		bindings.addStorageBuffer("BVHBuffer", 0);
		m_techniqueRayTracedShadows = Gfx_CreateTechnique(GfxTechniqueDesc(cs.get(), &bindings));
	}

	{
		GfxVertexFormatRef vf;
		vf.takeover(Gfx_CreateVertexFormat(GfxVertexFormatDesc()));

		GfxVertexShaderRef vs;
		vs.takeover(Gfx_CreateVertexShader(shaderFromFile("Shaders/Blit.vert.spv", GfxShaderType::Vertex)));

		{
			GfxPixelShaderRef ps;
			ps.takeover(Gfx_CreatePixelShader(shaderFromFile("Shaders/Combine.frag.spv", GfxShaderType::Pixel)));

			GfxShaderBindings bindings;
			bindings.addConstantBuffer("Constants", 0);
			bindings.addSampler("gbufferBaseColorSampler", 0);
			bindings.addSampler("gbufferNormalSampler", 0);
			bindings.addSampler("shadowMaskSampler", 0);

			m_techniqueCombine = Gfx_CreateTechnique(GfxTechniqueDesc(ps.get(), vs.get(), vf.get(), &bindings));
		}
	}

	{
		GfxBufferDesc cbDesc(GfxBufferType::Constant, GfxBufferMode::Temporary, GfxFormat_Unknown, 1, sizeof(ModelConstants));
		m_modelGlobalConstantBuffer.takeover(Gfx_CreateBuffer(cbDesc));
	}

	{
		GfxBufferDesc cbDesc(GfxBufferType::Constant, GfxBufferMode::Temporary, GfxFormat_Unknown, 1, sizeof(RayTracingConstants));
		m_rayTracingConstantBuffer.takeover(Gfx_CreateBuffer(cbDesc));
	}

	if (g_appConfig.argc >= 2)
	{
		const char* modelFilename = g_appConfig.argv[1];
		m_statusString = std::string("Model: ") + modelFilename;
		m_valid = loadModel(modelFilename);

		Vec3 center = m_boundingBox.center();
		Vec3 dimensions = m_boundingBox.dimensions();
		float longestSide = dimensions.reduceMax();

		if (longestSide != 0)
		{
			m_cameraScale = longestSide / 100.0f;
			// float scale = 100.0f / longestSide;
			//m_worldTransform = Mat4::scaleTranslate(scale, -center*scale);
			//m_cameraMan.setMoveSpeed();
		}

		m_boundingBox.m_min = m_worldTransform * m_boundingBox.m_min;
		m_boundingBox.m_max = m_worldTransform * m_boundingBox.m_max;
	}
	else
	{
		m_statusString = "Usage: RayTracedShadows <filename.obj>";
	}

	float aspect = m_window->getAspect();
	float fov = 1.0f;

	m_camera = Camera(aspect, fov, 0.25f, 10000.0f);
	m_camera.lookAt(Vec3(m_boundingBox.m_max) + Vec3(2.0f), m_boundingBox.center());
	m_interpolatedCamera = m_camera;

	m_lightCamera.lookAt(Vec3(0.0f), Vec3(1.0f));

	Log::message("Initialization complete");
}

RayTracedShadowsApp::~RayTracedShadowsApp()
{
	m_windowEvents.setOwner(nullptr);

	Gfx_Release(m_defaultWhiteTexture);
	Gfx_Release(m_vertexBuffer);
	Gfx_Release(m_indexBuffer);
	Gfx_Release(m_techniqueModel);
	Gfx_Release(m_techniqueRayTracedShadows);
	Gfx_Release(m_techniqueCombine);
}

void RayTracedShadowsApp::update()
{
	TimingScope timingScope(m_stats.cpuTotal);

	m_stats.gpuGbuffer.add(Gfx_Stats().customTimer[Timestamp_Gbuffer]);
	m_stats.gpuShadows.add(Gfx_Stats().customTimer[Timestamp_Shadows]);
	m_stats.gpuTotal.add(Gfx_Stats().lastFrameGpuTime);

	Gfx_ResetStats();

	const float dt = (float)m_timer.time();
	m_timer.reset();

	bool wantResize = false;
	Tuple2i pendingSize;

	for (const WindowEvent& e : m_windowEvents)
	{
		switch (e.type)
		{
		case WindowEventType_Resize:
			wantResize = true;
			pendingSize = Tuple2i{ (int)e.width, (int)e.height };
			break;

		case WindowEventType_MouseDown:
			if (e.button == 1)
			{
				m_prevMousePos = m_window->getMouseState().pos;
			}
			break;

		case WindowEventType_Scroll:
			if (e.scroll.y > 0)
			{
				m_cameraScale *= 1.25f;
			}
			else
			{
				m_cameraScale *= 0.9f;
			}
			Log::message("Camera scale: %f", m_cameraScale);
			break;
		default:
			break;
		}
	}

	if (wantResize)
	{
		createRenderTargets(pendingSize);
	}

	float clipNear = 0.25f * m_cameraScale;
	float clipFar = 10000.0f * m_cameraScale;
	m_camera.setClip(clipNear, clipFar);
	m_camera.setAspect(m_window->getAspect());
	m_cameraMan.setMoveSpeed(20.0f * m_cameraScale);
	m_cameraMan.update(&m_camera, dt,
		m_window->getKeyboardState(),
		m_window->getMouseState());
	

	m_interpolatedCamera.blendTo(m_camera, 0.1f, 0.125f);

	if (m_window->getMouseState().buttons[1])
	{
		Vec2 mouseDelta = m_window->getMouseState().pos - m_prevMousePos;
		if (mouseDelta != Vec2(0.0f))
		{
			mouseDelta *= 0.005f;
			m_lightCamera.rotateOnAxis(mouseDelta.x, Vec3(0, 1, 0));
			m_lightCamera.rotateOnAxis(-mouseDelta.y, m_lightCamera.getRight());
		}
	}

	m_prevMousePos = m_window->getMouseState().pos;

	m_windowEvents.clear();

	const GfxCapability& caps = Gfx_GetCapability();

	Mat4 matView = m_interpolatedCamera.buildViewMatrix();
	Mat4 matProj = m_interpolatedCamera.buildProjMatrix(caps.projectionFlags);
	m_matViewProj = matView * matProj;
	m_matViewProjInv = m_matViewProj.inverse();

	render();
}


void RayTracedShadowsApp::createRenderTargets(Tuple2i size)
{
	GfxTextureDesc desc;
	desc.type = TextureType::Tex2D;
	desc.width = size.x;
	desc.height = size.y;
	desc.depth = 1;
	desc.mips = 1;

	desc.format = GfxFormat_RGBA8_Unorm;
	desc.usage = GfxUsageFlags::RenderTarget | GfxUsageFlags::ShaderResource;
	m_gbufferBaseColor.takeover(Gfx_CreateTexture(desc));

	desc.format = GfxFormat_RGBA16_Float;
	desc.usage = GfxUsageFlags::RenderTarget | GfxUsageFlags::ShaderResource;
	m_gbufferNormal.takeover(Gfx_CreateTexture(desc));

	desc.format = GfxFormat_RGBA32_Float;
	desc.usage = GfxUsageFlags::RenderTarget | GfxUsageFlags::ShaderResource;
	m_gbufferPosition.takeover(Gfx_CreateTexture(desc));

	desc.format = GfxFormat_D32_Float;
	desc.usage = GfxUsageFlags::DepthStencil | GfxUsageFlags::ShaderResource;
	m_gbufferDepth.takeover(Gfx_CreateTexture(desc));

	desc.format = GfxFormat_R8_Unorm;
	desc.usage = GfxUsageFlags::ShaderResource | GfxUsageFlags::StorageImage;
	m_shadowMask.takeover(Gfx_CreateTexture(desc));
}

void RayTracedShadowsApp::render()
{
	if (m_valid)
	{
		renderGbuffer();
		renderShadowMask();
	}

	Gfx_AddImageBarrier(m_ctx, m_gbufferBaseColor, GfxResourceState_ShaderRead);
	Gfx_AddImageBarrier(m_ctx, m_gbufferNormal, GfxResourceState_ShaderRead);
	Gfx_AddImageBarrier(m_ctx, m_shadowMask, GfxResourceState_ShaderRead);

	GfxPassDesc passDesc;
	passDesc.clearDepth = 1.0f;
	passDesc.clearColors[0] = ColorRGBA8(11, 22, 33);
	passDesc.flags = GfxPassFlags::ClearAll;
	Gfx_BeginPass(m_ctx, passDesc);

	// Combine gbuffer with shadow mask
	if (m_valid)
	{
		Gfx_SetDepthStencilState(m_ctx, m_depthStencilStates.disable);
		Gfx_SetBlendState(m_ctx, m_blendStates.opaque);
		Gfx_SetTechnique(m_ctx, m_techniqueCombine);
		Gfx_SetTexture(m_ctx, GfxStage::Pixel, 0, m_gbufferBaseColor, m_samplerStates.pointClamp);
		Gfx_SetTexture(m_ctx, GfxStage::Pixel, 1, m_gbufferNormal, m_samplerStates.pointClamp);
		Gfx_SetTexture(m_ctx, GfxStage::Pixel, 2, m_shadowMask, m_samplerStates.pointClamp);
		Gfx_SetConstantBuffer(m_ctx, 0, m_rayTracingConstantBuffer);
		Gfx_Draw(m_ctx, 0, 3);
	}

	// Draw UI on top
	{
		TimingScope timingScope(m_stats.cpuUI);

		Gfx_SetBlendState(m_ctx, m_blendStates.lerp);
		Gfx_SetDepthStencilState(m_ctx, m_depthStencilStates.disable);

		m_prim->begin2D(m_window->getSize());

		m_font->setScale(2.0f);
		m_font->draw(m_prim, Vec2(10.0f), m_statusString.c_str());

		double raysTraced = m_window->getWidth() * m_window->getHeight();
		double raysPerSecond = raysTraced / m_stats.gpuShadows.get();

		m_font->setScale(1.0f);
		char timingString[1024];
		const GfxStats& stats = Gfx_Stats();
		sprintf_s(timingString,
			"Draw calls: %d\n"
			"Vertices: %d\n"
			"GPU shadows: %.2f ms\n"
			"mrays / sec: %.4f\n"
			"GPU total: %.2f ms\n"
			"CPU time: %.2f ms\n"
			"> Model: %.2f ms\n"
			"> UI: %.2f ms",
			stats.drawCalls,
			stats.vertices,
			m_stats.gpuShadows.get() * 1000.0f,
			raysPerSecond / 1000000.0,
			m_stats.gpuTotal.get() * 1000.0f,
			m_stats.cpuTotal.get() * 1000.0f,
			m_stats.cpuModel.get() * 1000.0f,
			m_stats.cpuUI.get() * 1000.0f);
		m_font->draw(m_prim, Vec2(10.0f, 30.0f), timingString);

		m_prim->end2D();
	}

	Gfx_EndPass(m_ctx);
}

void RayTracedShadowsApp::renderGbuffer()
{
	GfxPassDesc passDesc;
	passDesc.clearDepth = 1.0f;
	passDesc.clearColors[0] = ColorRGBA::Black();
	passDesc.clearColors[1] = ColorRGBA::Black();
	passDesc.color[0] = m_gbufferBaseColor.get();
	passDesc.color[1] = m_gbufferNormal.get();
	passDesc.color[2] = m_gbufferPosition.get();
	passDesc.depth = m_gbufferDepth.get();
	passDesc.flags = GfxPassFlags::ClearAll;
	Gfx_BeginPass(m_ctx, passDesc);
	Gfx_BeginTimer(m_ctx, Timestamp_Gbuffer);

	ModelConstants constants;
	constants.matViewProj = m_matViewProj.transposed();
	constants.matWorld = m_worldTransform.transposed();
	constants.cameraPosition = Vec4(m_interpolatedCamera.getPosition(), 0.0f);

	Gfx_UpdateBuffer(m_ctx, m_modelGlobalConstantBuffer, &constants, sizeof(constants));

	Gfx_SetViewport(m_ctx, GfxViewport(m_window->getSize()));
	Gfx_SetScissorRect(m_ctx, m_window->getSize());

	Gfx_SetDepthStencilState(m_ctx, m_depthStencilStates.writeLessEqual);

	if (m_valid)
	{
		TimingScope timingScope(m_stats.cpuModel);

		Gfx_SetBlendState(m_ctx, m_blendStates.opaque);

		Gfx_SetTechnique(m_ctx, m_techniqueModel);
		Gfx_SetVertexStream(m_ctx, 0, m_vertexBuffer);
		Gfx_SetIndexStream(m_ctx, m_indexBuffer);
		Gfx_SetConstantBuffer(m_ctx, 0, m_modelGlobalConstantBuffer);

		for (const MeshSegment& segment : m_segments)
		{
			GfxTexture texture = m_defaultWhiteTexture;

			const Material& material = (segment.material == 0xFFFFFFFF) ? m_defaultMaterial : m_materials[segment.material];
			if (material.albedoTexture.valid())
			{
				texture = material.albedoTexture.get();
			}
			Gfx_SetConstantBuffer(m_ctx, 1, material.constantBuffer);

			Gfx_SetTexture(m_ctx, GfxStage::Pixel, 0, texture, m_samplerStates.anisotropicWrap);
			Gfx_DrawIndexed(m_ctx, segment.indexCount, segment.indexOffset, 0, m_vertexCount);
		}
	}

	Gfx_EndTimer(m_ctx, Timestamp_Gbuffer);
	Gfx_EndPass(m_ctx);
}

void RayTracedShadowsApp::renderShadowMask()
{
	Gfx_BeginTimer(m_ctx, Timestamp_Shadows);

	const GfxTextureDesc& desc = Gfx_GetTextureDesc(m_shadowMask);

	RayTracingConstants constants;
	constants.cameraDirection = Vec4(m_interpolatedCamera.getForward(), 0.0f);
	constants.lightDirection = Vec4(m_lightCamera.getForward(), 0.0f);
	constants.cameraPosition = Vec4(m_interpolatedCamera.getPosition(), 0.0f);
	constants.renderTargetSize = Vec4((float)desc.width, (float)desc.height, 1.0f / desc.width, 1.0f / desc.height);
	Gfx_UpdateBuffer(m_ctx, m_rayTracingConstantBuffer, constants);

	Gfx_SetConstantBuffer(m_ctx, 0, m_rayTracingConstantBuffer);
	Gfx_SetTexture(m_ctx, GfxStage::Compute, 0, m_gbufferPosition);
	Gfx_SetStorageImage(m_ctx, GfxStage::Compute, 0, m_shadowMask);
	Gfx_SetStorageBuffer(m_ctx, GfxStage::Compute, 0, m_bvhBuffer);
	Gfx_SetTechnique(m_ctx, m_techniqueRayTracedShadows);

	u32 w = divUp(desc.width, 8);
	u32 h = divUp(desc.height, 8);
	Gfx_Dispatch(m_ctx, w, h, 1);

	Gfx_EndTimer(m_ctx, Timestamp_Shadows);
}

static std::string directoryFromFilename(const std::string& filename)
{
	size_t pos = filename.find_last_of("/\\");
	if (pos != std::string::npos)
	{
		return filename.substr(0, pos + 1);
	}
	else
	{
		return std::string();
	}
}

GfxRef<GfxTexture> RayTracedShadowsApp::loadTexture(const std::string& filename)
{
	auto it = m_textures.find(filename);
	if (it == m_textures.end())
	{
		Log::message("Loading texture '%s'", filename.c_str());

		int w, h, comp;
		stbi_set_flip_vertically_on_load(true);
		u8* pixels = stbi_load(filename.c_str(), &w, &h, &comp, 4);

		GfxRef<GfxTexture> texture;

		if (pixels)
		{
			std::vector<std::unique_ptr<u8>> mips;
			mips.reserve(16);

			std::vector<GfxTextureData> textureData;
			textureData.reserve(16);
			textureData.push_back(GfxTextureData(pixels));

			u32 mipWidth = w;
			u32 mipHeight = h;

			while (mipWidth != 1 && mipHeight != 1)
			{
				u32 nextMipWidth = max<u32>(1, mipWidth / 2);
				u32 nextMipHeight = max<u32>(1, mipHeight / 2);

				u8* nextMip = new u8[nextMipWidth * nextMipHeight * 4];
				mips.push_back(std::unique_ptr<u8>(nextMip));

				const u32 mipPitch = mipWidth * 4;
				const u32 nextMipPitch = nextMipWidth * 4;
				int resizeResult = stbir_resize_uint8(
					(const u8*)textureData.back().pixels, mipWidth, mipHeight, mipPitch,
					nextMip, nextMipWidth, nextMipHeight, nextMipPitch, 4);
				RUSH_ASSERT(resizeResult);

				textureData.push_back(GfxTextureData(nextMip, (u32)textureData.size()));

				mipWidth = nextMipWidth;
				mipHeight = nextMipHeight;
			}

			GfxTextureDesc desc = GfxTextureDesc::make2D(w, h);
			desc.mips = (u32)textureData.size();

			texture.takeover(Gfx_CreateTexture(desc, textureData.data(), (u32)textureData.size()));
			m_textures.insert(std::make_pair(filename, texture));

			stbi_image_free(pixels);
		}

		return texture;
	}
	else
	{
		return it->second;
	}
}

bool RayTracedShadowsApp::loadModel(const char* filename)
{
	Log::message("Loading model '%s'", filename);

	std::vector<tinyobj::shape_t> shapes;
	std::vector<tinyobj::material_t> materials;
	std::string errors;

	std::string directory = directoryFromFilename(filename);

	bool loaded = tinyobj::LoadObj(shapes, materials, errors, filename, directory.c_str());
	if (!loaded)
	{
		Log::error("Could not load model from '%s'\n%s\n", filename, errors.c_str());
		return false;
	}

	const GfxBufferDesc materialCbDesc(GfxBufferType::Constant, GfxBufferMode::Static, GfxFormat_Unknown, 1, sizeof(MaterialConstants));
	for (auto& objMaterial : materials)
	{
		MaterialConstants constants;
		constants.baseColor.x = objMaterial.diffuse[0];
		constants.baseColor.y = objMaterial.diffuse[1];
		constants.baseColor.z = objMaterial.diffuse[2];

		Material material;
		if (!objMaterial.diffuse_texname.empty())
		{
			material.albedoTexture = loadTexture(directory + objMaterial.diffuse_texname);
		}

		material.constantBuffer.takeover(Gfx_CreateBuffer(materialCbDesc, &constants));

		m_materials.push_back(material);
	}

	{
		MaterialConstants constants;
		constants.baseColor = Vec4(1.0f);
		m_defaultMaterial.constantBuffer.takeover(Gfx_CreateBuffer(materialCbDesc, &constants));
		m_defaultMaterial.albedoTexture.retain(m_defaultWhiteTexture);
	}

	std::vector<Vertex> vertices;
	std::vector<u32> indices;

	m_boundingBox.expandInit();

	for (const auto& shape : shapes)
	{
		u32 firstVertex = (u32)vertices.size();
		const auto& mesh = shape.mesh;

		const u32 vertexCount = (u32)mesh.positions.size() / 3;

		const bool haveTexcoords = !mesh.texcoords.empty();
		const bool haveNormals = mesh.positions.size() == mesh.normals.size();

		for (u32 i = 0; i < vertexCount; ++i)
		{
			Vertex v;

			v.position.x = mesh.positions[i * 3 + 0];
			v.position.y = mesh.positions[i * 3 + 1];
			v.position.z = mesh.positions[i * 3 + 2];

			m_boundingBox.expand(v.position);

			if (haveTexcoords)
			{
				v.texcoord.x = mesh.texcoords[i * 2 + 0];
				v.texcoord.y = mesh.texcoords[i * 2 + 1];
			}
			else
			{
				v.texcoord = Vec2(0.0f);
			}

			if (haveNormals)
			{
				v.normal.x = mesh.normals[i * 3 + 0];
				v.normal.y = mesh.normals[i * 3 + 1];
				v.normal.z = mesh.normals[i * 3 + 2];
			}
			else
			{
				v.normal = Vec3(0.0);
			}

			v.position.x = -v.position.x;
			v.normal.x = -v.normal.x;

			vertices.push_back(v);
		}

		if (!haveNormals)
		{
			const u32 triangleCount = (u32)mesh.indices.size() / 3;
			for (u32 i = 0; i < triangleCount; ++i)
			{
				u32 idxA = firstVertex + mesh.indices[i * 3 + 0];
				u32 idxB = firstVertex + mesh.indices[i * 3 + 2];
				u32 idxC = firstVertex + mesh.indices[i * 3 + 1];

				Vec3 a = vertices[idxA].position;
				Vec3 b = vertices[idxB].position;
				Vec3 c = vertices[idxC].position;

				Vec3 normal = cross(b - a, c - b);

				normal = normalize(normal);

				vertices[idxA].normal += normal;
				vertices[idxB].normal += normal;
				vertices[idxC].normal += normal;
			}

			for (u32 i = firstVertex; i < (u32)vertices.size(); ++i)
			{
				vertices[i].normal = normalize(vertices[i].normal);
			}
		}

		u32 currentMaterialId = 0xFFFFFFFF;

		const u32 triangleCount = (u32)mesh.indices.size() / 3;
		for (u32 triangleIt = 0; triangleIt < triangleCount; ++triangleIt)
		{
			if (mesh.material_ids[triangleIt] != currentMaterialId || m_segments.empty())
			{
				currentMaterialId = mesh.material_ids[triangleIt];
				m_segments.push_back(MeshSegment());
				m_segments.back().material = currentMaterialId;
				m_segments.back().indexOffset = (u32)indices.size();
				m_segments.back().indexCount = 0;
			}

			indices.push_back(mesh.indices[triangleIt * 3 + 0] + firstVertex);
			indices.push_back(mesh.indices[triangleIt * 3 + 2] + firstVertex);
			indices.push_back(mesh.indices[triangleIt * 3 + 1] + firstVertex);

			m_segments.back().indexCount += 3;
		}

		m_vertexCount = (u32)vertices.size();
		m_indexCount = (u32)indices.size();
	}

	GfxBufferDesc vbDesc(GfxBufferType::Vertex, GfxBufferMode::Static, GfxFormat_Unknown, m_vertexCount, sizeof(Vertex));
	m_vertexBuffer = Gfx_CreateBuffer(vbDesc, vertices.data());

	GfxBufferDesc ibDesc(GfxBufferType::Index, GfxBufferMode::Static, GfxFormat_R32_Uint, m_indexCount, 4);
	m_indexBuffer = Gfx_CreateBuffer(ibDesc, indices.data());

	Log::message("Building BVH");

	{
		BVHBuilder bvhBuilder;
		bvhBuilder.build(
			reinterpret_cast<float*>(vertices.data()),
			sizeof(Vertex) / sizeof(float),
			indices.data(),
			(u32)indices.size() / 3);

		GfxBufferDesc desc;
		desc.type = GfxBufferType::Storage;
		desc.mode = GfxBufferMode::Static;
		desc.format = GfxFormat_Unknown;
		desc.stride = sizeof(bvhBuilder.m_packedNodes[0]);
		desc.count = (u32)bvhBuilder.m_packedNodes.size();
		m_bvhBuffer.takeover(Gfx_CreateBuffer(desc, bvhBuilder.m_packedNodes.data()));
	}

	return true;
}
