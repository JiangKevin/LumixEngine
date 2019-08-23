#include "renderer.h"

#include "engine/array.h"
#include "engine/command_line_parser.h"
#include "engine/crc32.h"
#include "engine/debug.h"
#include "engine/engine.h"
#include "engine/log.h"
#include "engine/job_system.h"
#include "engine/mt/atomic.h"
#include "engine/mt/sync.h"
#include "engine/mt/task.h"
#include "engine/os.h"
#include "engine/profiler.h"
#include "engine/reflection.h"
#include "engine/resource_manager.h"
#include "engine/string.h"
#include "engine/universe/component.h"
#include "engine/universe/universe.h"
#include "renderer/font.h"
#include "renderer/material.h"
#include "renderer/model.h"
#include "renderer/pipeline.h"
#include "renderer/particle_system.h"
#include "renderer/render_scene.h"
#include "renderer/shader.h"
#include "renderer/terrain.h"
#include "renderer/texture.h"


#include <Windows.h>
#undef near
#undef far
#include "gl/GL.h"
#include "ffr/ffr.h"
#include <stdio.h>

#define FFR_GL_IMPORT(prototype, name) static prototype name;
#define FFR_GL_IMPORT_TYPEDEFS

#include "ffr/gl_ext.h"

#define CHECK_GL(gl) \
	do { \
		gl; \
		GLenum err = glGetError(); \
		if (err != GL_NO_ERROR) { \
			logError("Renderer") << "OpenGL error " << err; \
		} \
	} while(0)

namespace Lumix
{


static const ComponentType MODEL_INSTANCE_TYPE = Reflection::getComponentType("model_instance");

enum { 
	TRANSIENT_BUFFER_INIT_SIZE = 1 * 1024 * 1024,
	MATERIAL_BUFFER_SIZE = 1 * 1024 * 1024
};


template <typename T>
struct RenderResourceManager : public ResourceManager
{
	RenderResourceManager(Renderer& renderer, IAllocator& allocator) 
		: ResourceManager(allocator)
		, m_renderer(renderer)
	{}


	Resource* createResource(const Path& path) override
	{
		return LUMIX_NEW(m_allocator, T)(path, *this, m_renderer, m_allocator);
	}


	void destroyResource(Resource& resource) override
	{
		LUMIX_DELETE(m_allocator, &resource);
	}

	Renderer& m_renderer;
};


struct GPUProfiler
{
	struct Query
	{
		StaticString<32> name;
		ffr::QueryHandle handle;
		u64 result;
		i64 profiler_link;
		bool is_end;
		bool is_frame;
	};


	GPUProfiler(IAllocator& allocator) 
		: m_queries(allocator)
		, m_pool(allocator)
		, m_gpu_to_cpu_offset(0)
	{
	}


	~GPUProfiler()
	{
		ASSERT(m_pool.empty());
		ASSERT(m_queries.empty());
	}


	u64 toCPUTimestamp(u64 gpu_timestamp) const
	{
		return u64(gpu_timestamp * (OS::Timer::getFrequency() / double(1'000'000'000))) + m_gpu_to_cpu_offset;
	}


	void init()
	{
		ffr::QueryHandle q = ffr::createQuery();
		ffr::queryTimestamp(q);
		const u64 cpu_timestamp = OS::Timer::getRawTimestamp();
		const u64 gpu_timestamp = ffr::getQueryResult(q);
		m_gpu_to_cpu_offset = cpu_timestamp - u64(gpu_timestamp * (OS::Timer::getFrequency() / double(1'000'000'000)));
		ffr::destroy(q);
	}


	void clear()
	{
		m_queries.clear();

		for(const ffr::QueryHandle h : m_pool) {
			ffr::destroy(h);
		}
		m_pool.clear();
	}


	ffr::QueryHandle allocQuery()
	{
		if(!m_pool.empty()) {
			const ffr::QueryHandle res = m_pool.back();
			m_pool.pop();
			return res;
		}
		return ffr::createQuery();
	}


	void beginQuery(const char* name, i64 profiler_link)
	{
		MT::CriticalSectionLock lock(m_mutex);
		Query& q = m_queries.emplace();
		q.profiler_link = profiler_link;
		q.name = name;
		q.is_end = false;
		q.is_frame = false;
		q.handle = allocQuery();
		ffr::queryTimestamp(q.handle);
	}


	void endQuery()
	{
		MT::CriticalSectionLock lock(m_mutex);
		Query& q = m_queries.emplace();
		q.is_end = true;
		q.is_frame = false;
		q.handle = allocQuery();
		ffr::queryTimestamp(q.handle);
	}


	void frame()
	{
		PROFILE_FUNCTION();
		MT::CriticalSectionLock lock(m_mutex);
		Query frame_query;
		frame_query.is_frame = true;
		m_queries.push(frame_query);
		while (!m_queries.empty()) {
			Query q = m_queries[0];
			if (q.is_frame) {
				Profiler::gpuFrame();
				m_queries.erase(0);
				continue;
			}
			
			if (!ffr::isQueryReady(q.handle)) break;

			if (q.is_end) {
				const u64 timestamp = toCPUTimestamp(ffr::getQueryResult(q.handle));
				Profiler::endGPUBlock(timestamp);
			}
			else {
				const u64 timestamp = toCPUTimestamp(ffr::getQueryResult(q.handle));
				Profiler::beginGPUBlock(q.name, timestamp, q.profiler_link);
			}
			m_pool.push(q.handle);
			m_queries.erase(0);
		}
	}


	Array<Query> m_queries;
	Array<ffr::QueryHandle> m_pool;
	MT::CriticalSection m_mutex;
	i64 m_gpu_to_cpu_offset;
};


struct BoneProperty : Reflection::IEnumProperty
{
	BoneProperty() 
	{ 
		name = "Bone"; 
	}


	void getValue(ComponentUID cmp, int index, OutputMemoryStream& stream) const override
	{
		RenderScene* scene = static_cast<RenderScene*>(cmp.scene);
		int value = scene->getBoneAttachmentBone((EntityRef)cmp.entity);
		stream.write(value);
	}


	void setValue(ComponentUID cmp, int index, InputMemoryStream& stream) const override
	{
		RenderScene* scene = static_cast<RenderScene*>(cmp.scene);
		int value = stream.read<int>();
		scene->setBoneAttachmentBone((EntityRef)cmp.entity, value);
	}


	EntityPtr getModelInstance(RenderScene* render_scene, EntityRef bone_attachment) const
	{
		EntityPtr parent_entity = render_scene->getBoneAttachmentParent(bone_attachment);
		if (!parent_entity.isValid()) return INVALID_ENTITY;
		return render_scene->getUniverse().hasComponent((EntityRef)parent_entity, MODEL_INSTANCE_TYPE) ? parent_entity : INVALID_ENTITY;
	}


	int getEnumValueIndex(ComponentUID cmp, int value) const override  { return value; }
	int getEnumValue(ComponentUID cmp, int index) const override { return index; }


	int getEnumCount(ComponentUID cmp) const override
	{
		RenderScene* render_scene = static_cast<RenderScene*>(cmp.scene);
		EntityPtr model_instance = getModelInstance(render_scene, (EntityRef)cmp.entity);
		if (!model_instance.isValid()) return 0;

		auto* model = render_scene->getModelInstanceModel((EntityRef)model_instance);
		if (!model || !model->isReady()) return 0;

		return model->getBoneCount();
	}


	const char* getEnumName(ComponentUID cmp, int index) const override
	{
		RenderScene* render_scene = static_cast<RenderScene*>(cmp.scene);
		EntityPtr model_instance = getModelInstance(render_scene, (EntityRef)cmp.entity);
		if (!model_instance.isValid()) return "";

		auto* model = render_scene->getModelInstanceModel((EntityRef)model_instance);
		if (!model) return "";

		return model->getBone(index).name.c_str();
	}
};


static void registerProperties(IAllocator& allocator)
{
	using namespace Reflection;

	static auto rotationModeDesc = enumDesciptor<Terrain::GrassType::RotationMode>(
		LUMIX_ENUM_VALUE(Terrain::GrassType::RotationMode::ALL_RANDOM),
		LUMIX_ENUM_VALUE(Terrain::GrassType::RotationMode::Y_UP),
		LUMIX_ENUM_VALUE(Terrain::GrassType::RotationMode::ALIGN_WITH_NORMAL)
	);
	registerEnum(rotationModeDesc);

	static auto render_scene = scene("renderer", 
		component("bone_attachment",
			property("Parent", LUMIX_PROP(RenderScene, BoneAttachmentParent)),
			property("Relative position", LUMIX_PROP(RenderScene, BoneAttachmentPosition)),
			property("Relative rotation", LUMIX_PROP(RenderScene, BoneAttachmentRotation), 
				RadiansAttribute()),
			BoneProperty()
		),
		component("environment_probe",
			property("Enabled", &RenderScene::isEnvironmentProbeEnabled, &RenderScene::enableEnvironmentProbe),
			property("Radius", LUMIX_PROP(RenderScene, EnvironmentProbeRadius)),
			property("Enabled reflection", &RenderScene::isEnvironmentProbeReflectionEnabled, &RenderScene::enableEnvironmentProbeReflection),
			property("Override global size", &RenderScene::isEnvironmentProbeCustomSize, &RenderScene::enableEnvironmentProbeCustomSize),
			var_property("Radiance size", &RenderScene::getEnvironmentProbe, &EnvironmentProbe::radiance_size),
			var_property("Irradiance size", &RenderScene::getEnvironmentProbe, &EnvironmentProbe::irradiance_size)
		),
		component("particle_emitter",
			property("Resource", LUMIX_PROP(RenderScene, ParticleEmitterPath),
				ResourceAttribute("Particle emitter (*.par)", ParticleEmitterResource::TYPE))
		),
		component("camera",
			var_property("FOV", &RenderScene::getCamera, &Camera::fov, RadiansAttribute()),
			var_property("Near", &RenderScene::getCamera, &Camera::near, MinAttribute(0)),
			var_property("Far", &RenderScene::getCamera, &Camera::far, MinAttribute(0)),
			var_property("Orthographic", &RenderScene::getCamera, &Camera::is_ortho),
			var_property("Orthographic size", &RenderScene::getCamera, &Camera::ortho_size, MinAttribute(0))
		),
		component("model_instance",
			property("Enabled", &RenderScene::isModelInstanceEnabled, &RenderScene::enableModelInstance),
			property("Source", LUMIX_PROP(RenderScene, ModelInstancePath),
				ResourceAttribute("Mesh (*.msh)", Model::TYPE))
		),
		component("environment",
			var_property("Color", &RenderScene::getEnvironment, &Environment::m_diffuse_color, ColorAttribute()),
			var_property("Intensity", &RenderScene::getEnvironment, &Environment::m_diffuse_intensity, MinAttribute(0)),
			var_property("Indirect intensity", &RenderScene::getEnvironment, &Environment::m_indirect_intensity, MinAttribute(0)),
			var_property("Fog density", &RenderScene::getEnvironment, &Environment::m_fog_density, ClampAttribute(0, 1)),
			var_property("Fog bottom", &RenderScene::getEnvironment, &Environment::m_fog_bottom),
			var_property("Fog height", &RenderScene::getEnvironment, &Environment::m_fog_height, MinAttribute(0)),
			var_property("Fog color", &RenderScene::getEnvironment, &Environment::m_fog_color, ColorAttribute()),
			property("Shadow cascades", LUMIX_PROP(RenderScene, ShadowmapCascades)),
			property("Cast shadows", LUMIX_PROP(RenderScene, EnvironmentCastShadows))
		),
		component("point_light",
			var_property("Cast shadows", &RenderScene::getPointLight, &PointLight::cast_shadows),
			var_property("Intensity", &RenderScene::getPointLight, &PointLight::intensity, MinAttribute(0)),
			var_property("FOV", &RenderScene::getPointLight, &PointLight::fov, ClampAttribute(0, 360), RadiansAttribute()),
			var_property("Attenuation", &RenderScene::getPointLight, &PointLight::attenuation_param, ClampAttribute(0, 100)),
			var_property("Color", &RenderScene::getPointLight, &PointLight::color, ColorAttribute()),
			property("Range", LUMIX_PROP(RenderScene, LightRange), MinAttribute(0))
		),
		component("text_mesh",
			property("Text", LUMIX_PROP(RenderScene, TextMeshText)),
			property("Font", LUMIX_PROP(RenderScene, TextMeshFontPath),
				ResourceAttribute("Font (*.ttf)", FontResource::TYPE)),
			property("Font Size", LUMIX_PROP(RenderScene, TextMeshFontSize)),
			property("Color", LUMIX_PROP(RenderScene, TextMeshColorRGBA),
				ColorAttribute()),
			property("Camera-oriented", &RenderScene::isTextMeshCameraOriented, &RenderScene::setTextMeshCameraOriented)
		),
		component("decal",
			property("Material", LUMIX_PROP(RenderScene, DecalMaterialPath),
				ResourceAttribute("Material (*.mat)", Material::TYPE)),
			property("Half extents", LUMIX_PROP(RenderScene, DecalHalfExtents), 
				MinAttribute(0))
		),
		component("terrain",
			property("Material", LUMIX_PROP(RenderScene, TerrainMaterialPath),
				ResourceAttribute("Material (*.mat)", Material::TYPE)),
			property("XZ scale", LUMIX_PROP(RenderScene, TerrainXZScale), 
				MinAttribute(0)),
			property("Height scale", LUMIX_PROP(RenderScene, TerrainYScale), 
				MinAttribute(0)),
			array("grass", &RenderScene::getGrassCount, &RenderScene::addGrass, &RenderScene::removeGrass,
				property("Mesh", LUMIX_PROP(RenderScene, GrassPath),
					ResourceAttribute("Mesh (*.msh)", Model::TYPE)),
				property("Distance", LUMIX_PROP(RenderScene, GrassDistance),
					MinAttribute(1)),
				property("Density", LUMIX_PROP(RenderScene, GrassDensity)),
				enum_property("Mode", LUMIX_PROP(RenderScene, GrassRotationMode), rotationModeDesc)
			)
		)
	);
	registerScene(render_scene);
}


struct RendererImpl final : public Renderer
{
	explicit RendererImpl(Engine& engine)
		: m_engine(engine)
		, m_allocator(engine.getAllocator())
		, m_texture_manager(*this, m_allocator)
		, m_pipeline_manager(*this, m_allocator)
		, m_model_manager(*this, m_allocator)
		, m_particle_emitter_manager(*this, m_allocator)
		, m_material_manager(*this, m_allocator)
		, m_shader_manager(*this, m_allocator)
		, m_font_manager(nullptr)
		, m_shader_defines(m_allocator)
		, m_vsync(true)
		, m_profiler(m_allocator)
		, m_layers(m_allocator)
		, m_cmd_queue(m_allocator)
		, m_material_buffer(m_allocator)
	{
		m_shader_defines.reserve(32);
		ffr::preinit(m_allocator);
	}


	~RendererImpl()
	{
		m_particle_emitter_manager.destroy();
		m_pipeline_manager.destroy();
		m_texture_manager.destroy();
		m_model_manager.destroy();
		m_material_manager.destroy();
		m_shader_manager.destroy();
		m_font_manager->destroy();
		LUMIX_DELETE(m_allocator, m_font_manager);

		frame();
	
		JobSystem::SignalHandle signal = JobSystem::INVALID_HANDLE;
		JobSystem::runEx(this, [](void* data) {
			RendererImpl* renderer = (RendererImpl*)data;
			ffr::destroy(renderer->m_transient[0].buffer);
			ffr::destroy(renderer->m_transient[1].buffer);
			ffr::destroy(renderer->m_material_buffer.buffer);
			renderer->m_profiler.clear();
			ffr::shutdown();
		}, &signal, m_prev_frame_job, 1);
		JobSystem::wait(signal);
	}


	void init() override
	{
		registerProperties(m_engine.getAllocator());
		char cmd_line[4096];
		OS::getCommandLine(Span(cmd_line));
		CommandLineParser cmd_line_parser(cmd_line);
		m_vsync = true;
		m_debug_opengl = false;
		while (cmd_line_parser.next()) {
			if (cmd_line_parser.currentEquals("-no_vsync")) {
				m_vsync = false;
			}
			else if (cmd_line_parser.currentEquals("-debug_opengl")) {
				m_debug_opengl = true;
			}
		}

		JobSystem::SignalHandle signal = JobSystem::INVALID_HANDLE;
		JobSystem::runEx(this, [](void* data) {
			PROFILE_BLOCK("init_render");
			RendererImpl& renderer = *(RendererImpl*)data;
			Engine& engine = renderer.getEngine();
			void* window_handle = engine.getPlatformData().window_handle;
			ffr::init(window_handle, renderer.m_debug_opengl);
			
			renderer.m_transient[0].buffer = ffr::allocBufferHandle();
			renderer.m_transient[1].buffer = ffr::allocBufferHandle();
			renderer.m_transient[0].offset = 0;
			renderer.m_transient[1].offset = 0;
			ffr::createBuffer(renderer.m_transient[0].buffer, 0, TRANSIENT_BUFFER_INIT_SIZE, nullptr);
			ffr::createBuffer(renderer.m_transient[1].buffer, 0, TRANSIENT_BUFFER_INIT_SIZE, nullptr);
			renderer.m_transient[0].ptr = (u8*)ffr::map(renderer.m_transient[0].buffer, TRANSIENT_BUFFER_INIT_SIZE);
			renderer.m_transient[0].size = TRANSIENT_BUFFER_INIT_SIZE;
			renderer.m_transient[1].size = TRANSIENT_BUFFER_INIT_SIZE;
			renderer.m_current_transient = 0;
			
			renderer.m_profiler.init();

			renderer.m_material_buffer.buffer = ffr::allocBufferHandle();
			renderer.m_material_buffer.data.resize(400);
			renderer.m_material_buffer.map.insert(0, 0);
			ffr::createBuffer(renderer.m_material_buffer.buffer, (u32)ffr::BufferFlags::UNIFORM_BUFFER, renderer.m_material_buffer.data.byte_size(), nullptr);

			const u32 max_mat_count = renderer.m_material_buffer.data.size();
			for (u32 i = 0; i < max_mat_count; ++i) {
				*(u32*)&renderer.m_material_buffer.data[i] = i + 1;
			}
			*(u32*)&renderer.m_material_buffer.data[max_mat_count - 1] = 0xffFFffFF;
			renderer.m_material_buffer.data[0].color = Vec4(1, 0, 1, 1);
		}, &signal, JobSystem::INVALID_HANDLE, 1);
		JobSystem::wait(signal);

		ResourceManagerHub& manager = m_engine.getResourceManager();
		m_pipeline_manager.create(PipelineResource::TYPE, manager);
		m_texture_manager.create(Texture::TYPE, manager);
		m_model_manager.create(Model::TYPE, manager);
		m_material_manager.create(Material::TYPE, manager);
		m_particle_emitter_manager.create(ParticleEmitterResource::TYPE, manager);
		m_shader_manager.create(Shader::TYPE, manager);
		m_font_manager = LUMIX_NEW(m_allocator, FontManager)(*this, m_allocator);
		m_font_manager->create(FontResource::TYPE, manager);

		RenderScene::registerLuaAPI(m_engine.getState());

		m_layers.emplace("default");
	}


	MemRef copy(const void* data, u32 size) override
	{
		MemRef mem = allocate(size);
		copyMemory(mem.data, data, size);
		return mem;
	}


	IAllocator& getAllocator() override
	{
		return m_allocator;
	}


	void free(const MemRef& memory) override
	{
		ASSERT(memory.own);
		m_allocator.deallocate(memory.data);
	}


	MemRef allocate(u32 size) override
	{
		MemRef ret;
		ret.size = size;
		ret.own = true;
		ret.data = m_allocator.allocate(size);
		return ret;
	}


	void beginProfileBlock(const char* name, i64 link) override
	{
		m_profiler.beginQuery(name, link);
	}


	void endProfileBlock() override
	{
		m_profiler.endQuery();
	}


	void getTextureImage(ffr::TextureHandle texture, int size, void* data) override
	{
		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override {
				PROFILE_FUNCTION();
				ffr::pushDebugGroup("get image data");
				ffr::getTextureImage(handle, size, buf);
				ffr::popDebugGroup();
			}

			ffr::TextureHandle handle;
			u32 size;
			void* buf;
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->handle = texture;
		cmd->size = size;
		cmd->buf = data;
		queue(cmd, 0);
	}


	void updateTexture(ffr::TextureHandle handle, u32 x, u32 y, u32 w, u32 h, ffr::TextureFormat format, const MemRef& mem) override
	{
		ASSERT(mem.size > 0);
		ASSERT(handle.isValid());

		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override {
				PROFILE_FUNCTION();
				ffr::update(handle, 0, x, y, w, h, format, mem.data);
				if (mem.own) {
					renderer->free(mem);
				}
			}

			ffr::TextureHandle handle;
			u32 x, y, w, h;
			ffr::TextureFormat format;
			MemRef mem;
			RendererImpl* renderer;
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->handle = handle;
		cmd->x = x;
		cmd->y = y;
		cmd->w = w;
		cmd->h = h;
		cmd->format = format;
		cmd->mem = mem;
		cmd->renderer = this;

		queue(cmd, 0);
	}


	ffr::TextureHandle loadTexture(const MemRef& memory, u32 flags, ffr::TextureInfo* info, const char* debug_name) override
	{
		ASSERT(memory.size > 0);

		const ffr::TextureHandle handle = ffr::allocTextureHandle();
		if (!handle.isValid()) return handle;

		if(info) {
			*info = ffr::getTextureInfo(memory.data);
		}

		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override {
				PROFILE_FUNCTION();
				ffr::loadTexture(handle, memory.data, memory.size, flags, debug_name);
				if(memory.own) {
					renderer->free(memory);
				}
			}

			StaticString<MAX_PATH_LENGTH> debug_name;
			ffr::TextureHandle handle;
			MemRef memory;
			u32 flags;
			RendererImpl* renderer; 
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->debug_name = debug_name;
		cmd->handle = handle;
		cmd->memory = memory;
		cmd->flags = flags;
		cmd->renderer = this;
		queue(cmd, 0);

		return handle;
	}


	TransientSlice allocTransient(u32 size) override
	{
		TransientSlice slice;
		size = (size + 15) & ~15;
		auto& transient = m_transient[m_current_transient];
		slice.buffer = transient.buffer;
		slice.offset = MT::atomicAdd(&transient .offset, size);
		if (slice.offset + size > transient .size) {
			logError("Renderer") << "Out of transient memory";
			slice.size = 0;
			slice.ptr = nullptr;
		}
		else {
			slice.size = size;
			slice.ptr = transient.ptr + slice.offset;
		}
		return slice;
	}
	
	ffr::BufferHandle getMaterialUniformBuffer() override {
		return m_material_buffer.buffer;
	}

	u32 createMaterialConstants(const MaterialConsts& data) override {
		const u32 hash = crc32(&data, sizeof(data));
		auto iter = m_material_buffer.map.find(hash);
		u32 idx;
		if(iter.isValid()) {
			idx = iter.value();
		}
		else {
			idx = m_material_buffer.first_free;
			if (idx == 0xffFFffFF) {
				++m_material_buffer.data[0].ref_count;
				return 0;
			}
			const u32 next_free = *(u32*)&m_material_buffer.data[m_material_buffer.first_free];
			memcpy(&m_material_buffer.data[m_material_buffer.first_free], &data, sizeof(data));
			m_material_buffer.data[m_material_buffer.first_free].ref_count = 0;
			m_material_buffer.first_free = next_free;
			ASSERT(next_free != 0xffFFffFF);
			m_material_buffer.dirty = true;
			m_material_buffer.map.insert(hash, idx);
		}
		++m_material_buffer.data[idx].ref_count;
		return idx;
	}

	void destroyMaterialConstants(u32 idx) override {
		--m_material_buffer.data[idx].ref_count;
		if(m_material_buffer.data[idx].ref_count == 0) {
			const u32 hash = crc32(&m_material_buffer.data[idx], sizeof(m_material_buffer.data[idx]));
			*(u32*)&m_material_buffer.data[idx] = m_material_buffer.first_free;
			m_material_buffer.first_free = idx;
			m_material_buffer.map.erase(hash);
		}
	}


	ffr::BufferHandle createBuffer(const MemRef& memory, u32 flags) override
	{
		ffr::BufferHandle handle = ffr::allocBufferHandle();
		if(!handle.isValid()) return handle;

		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override {
				PROFILE_FUNCTION();
				ffr::createBuffer(handle, flags, memory.size, memory.data);
				if (memory.own) {
					renderer->free(memory);
				}
			}

			ffr::BufferHandle handle;
			MemRef memory;
			u32 flags;
			ffr::TextureFormat format;
			Renderer* renderer;
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->handle = handle;
		cmd->memory = memory;
		cmd->renderer = this;
		cmd->flags = flags;
		queue(cmd, 0);

		return handle;
	}

	
	u8 getLayersCount() const override
	{
		return (u8)m_layers.size();
	}


	const char* getLayerName(u8 layer) const override
	{
		return m_layers[layer];
	}


	u8 getLayerIdx(const char* name) override
	{
		for(int i = 0; i < m_layers.size(); ++i) {
			if(m_layers[i] == name) return i;
		}
		m_layers.emplace(name);
		return m_layers.size() - 1;
	}


	void runInRenderThread(void* user_ptr, void (*fnc)(Renderer& renderer, void*)) override
	{
		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override { 
				PROFILE_FUNCTION();
				fnc(*renderer, ptr); 
			}

			void* ptr;
			void (*fnc)(Renderer&, void*);
			Renderer* renderer;

		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->fnc = fnc;
		cmd->ptr = user_ptr;
		cmd->renderer = this;
		queue(cmd, 0);
	}

	
	void destroy(ffr::ProgramHandle program) override
	{
		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override { 
				PROFILE_FUNCTION();
				ffr::destroy(program); 
			}

			ffr::ProgramHandle program;
			RendererImpl* renderer;
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->program = program;
		cmd->renderer = this;
		queue(cmd, 0);
	}

	void destroy(ffr::BufferHandle buffer) override
	{
		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override { 
				PROFILE_FUNCTION();
				ffr::destroy(buffer);
			}

			ffr::BufferHandle buffer;
			RendererImpl* renderer;
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->buffer = buffer;
		cmd->renderer = this;
		queue(cmd, 0);
	}


	ffr::TextureHandle createTexture(u32 w, u32 h, u32 depth, ffr::TextureFormat format, u32 flags, const MemRef& memory, const char* debug_name) override
	{
		ffr::TextureHandle handle = ffr::allocTextureHandle();
		if(!handle.isValid()) return handle;

		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override
			{
				PROFILE_FUNCTION();
				ffr::createTexture(handle, w, h, depth, format, flags, memory.data, debug_name);
				if (memory.own) renderer->free(memory);
			}

			StaticString<MAX_PATH_LENGTH> debug_name;
			ffr::TextureHandle handle;
			MemRef memory;
			u32 w;
			u32 h;
			u32 depth;
			ffr::TextureFormat format;
			Renderer* renderer;
			u32 flags;
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->debug_name = debug_name;
		cmd->handle = handle;
		cmd->memory = memory;
		cmd->format = format;
		cmd->flags = flags;
		cmd->w = w;
		cmd->h = h;
		cmd->depth = depth;
		cmd->renderer = this;
		queue(cmd, 0);

		return handle;
	}


	void destroy(ffr::TextureHandle tex)
	{
		ASSERT(tex.isValid());
		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override { 
				PROFILE_FUNCTION();
				ffr::destroy(texture); 
			}

			ffr::TextureHandle texture;
			RendererImpl* renderer;
		};

		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		cmd->texture = tex;
		cmd->renderer = this;
		queue(cmd, 0);
	}


	void queue(RenderJob* cmd, i64 profiler_link) override
	{
		cmd->profiler_link = profiler_link;
		
		m_cmd_queue.push(cmd);

		JobSystem::wait(m_transient_ready);
		JobSystem::run(cmd, [](void* data){
			RenderJob* cmd = (RenderJob*)data;
			PROFILE_BLOCK("setup_render_job");
			cmd->setup();
		}, &m_setup_jobs_done);
	}


	ResourceManager& getTextureManager() override { return m_texture_manager; }
	FontManager& getFontManager() override { return *m_font_manager; }

	void createScenes(Universe& ctx) override
	{
		auto* scene = RenderScene::createInstance(*this, m_engine, ctx, m_allocator);
		ctx.addScene(scene);
	}


	void destroyScene(IScene* scene) override { RenderScene::destroyInstance(static_cast<RenderScene*>(scene)); }
	const char* getName() const override { return "renderer"; }
	Engine& getEngine() override { return m_engine; }
	int getShaderDefinesCount() const override { return m_shader_defines.size(); }
	const char* getShaderDefine(int define_idx) const override { return m_shader_defines[define_idx]; }

	void makeScreenshot(const Path& filename) override {  }
	void resize(int w, int h) override {  }


	u8 getShaderDefineIdx(const char* define) override
	{
		MT::CriticalSectionLock lock(m_shader_defines_mutex);
		for (int i = 0; i < m_shader_defines.size(); ++i)
		{
			if (m_shader_defines[i] == define)
			{
				return i;
			}
		}

		if (m_shader_defines.size() >= MAX_SHADER_DEFINES) {
			ASSERT(false);
			logError("Renderer") << "Too many shader defines.";
		}

		m_shader_defines.emplace(define);
		ASSERT(m_shader_defines.size() <= 32); // m_shader_defines are reserved in renderer constructor, so getShaderDefine() is MT safe
		return m_shader_defines.size() - 1;
	}


	void startCapture() override
	{
		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override { 
				PROFILE_FUNCTION();
				ffr::startCapture();
			}
		};
		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		queue(cmd, 0);
	}


	void stopCapture() override
	{
		struct Cmd : RenderJob {
			void setup() override {}
			void execute() override { 
				PROFILE_FUNCTION();
				ffr::stopCapture();
			}
		};
		Cmd* cmd = LUMIX_NEW(m_allocator, Cmd);
		queue(cmd, 0);
	}

	struct RenderFrameData {
		RenderFrameData(RendererImpl& renderer) 
			: m_renderer(renderer)
			, m_jobs(renderer.m_allocator)
		{
			renderer.m_cmd_queue.swap(m_jobs);
		}

		void render() {
			auto& current_transient = m_renderer.m_transient[m_renderer.m_current_transient];
			m_renderer.m_current_transient = (m_renderer.m_current_transient + 1) % 2;
			auto& next_transient = m_renderer.m_transient[m_renderer.m_current_transient];
			
			ffr::unmap(current_transient.buffer);
			current_transient.ptr = nullptr;

			if ((u32)next_transient.offset > next_transient.size) {
				next_transient.size = nextPow2(next_transient.offset);
				ffr::destroy(next_transient.buffer);
				next_transient.buffer = ffr::allocBufferHandle();
				ffr::createBuffer(next_transient.buffer, 0, next_transient.size, nullptr);
			}

			ASSERT(!next_transient.ptr);
			next_transient.ptr = (u8*)ffr::map(next_transient.buffer, next_transient.size);
			next_transient.offset = 0;
			JobSystem::decSignal(m_renderer.m_transient_ready);

			if (m_renderer.m_material_buffer.dirty) {
				ffr::update(m_renderer.m_material_buffer.buffer, m_renderer.m_material_buffer.data.begin(), m_renderer.m_material_buffer.data.byte_size());
				m_renderer.m_material_buffer.dirty = false;
			}
			for (RenderJob* job : m_jobs) {
				PROFILE_BLOCK("execute_render_job");
				Profiler::blockColor(0xaa, 0xff, 0xaa);
				Profiler::link(job->profiler_link);
				job->execute();
				LUMIX_DELETE(m_renderer.m_allocator, job);
			}

			PROFILE_BLOCK("swap buffers");
			JobSystem::enableBackupWorker(true);
			
			ffr::swapBuffers(m_window_size.x, m_window_size.y);
			
			ffr::FenceHandle fence;
			fence = ffr::createFence();
			ffr::waitClient(fence);
			ffr::destroy(fence);
			
			JobSystem::enableBackupWorker(false);
			m_renderer.m_profiler.frame();
			LUMIX_DELETE(m_renderer.m_allocator, this);
		}

		RendererImpl& m_renderer;
		Array<RenderJob*> m_jobs;
		OS::Point m_window_size;
	};

	void frame() override
	{
		PROFILE_FUNCTION();
		
		JobSystem::wait(m_setup_jobs_done);
		m_setup_jobs_done = JobSystem::INVALID_HANDLE;
		JobSystem::wait(m_prev_frame_job);
		m_prev_frame_job = JobSystem::INVALID_HANDLE;

		RenderFrameData* data = LUMIX_NEW(m_allocator, RenderFrameData)(*this);
		const void* window_handle = m_engine.getPlatformData().window_handle;
		data->m_window_size = OS::getWindowClientSize((OS::WindowHandle)window_handle);
		JobSystem::incSignal(&m_transient_ready);

		JobSystem::runEx(data, [](void* ptr){
			auto* data = (RenderFrameData*)ptr;
			data->render();
		}, &m_prev_frame_job, JobSystem::INVALID_HANDLE, 1);
	}

	Engine& m_engine;
	IAllocator& m_allocator;
	Array<StaticString<32>> m_shader_defines;
	MT::CriticalSection m_shader_defines_mutex;
	Array<StaticString<32>> m_layers;
	FontManager* m_font_manager;
	MaterialManager m_material_manager;
	RenderResourceManager<Model> m_model_manager;
	RenderResourceManager<ParticleEmitterResource> m_particle_emitter_manager;
	RenderResourceManager<PipelineResource> m_pipeline_manager;
	RenderResourceManager<Shader> m_shader_manager;
	RenderResourceManager<Texture> m_texture_manager;
	bool m_vsync;
	bool m_debug_opengl = false;
	JobSystem::SignalHandle m_prev_frame_job = JobSystem::INVALID_HANDLE;
	JobSystem::SignalHandle m_setup_jobs_done = JobSystem::INVALID_HANDLE;
	Array<RenderJob*> m_cmd_queue;

	struct {
		ffr::BufferHandle buffer;
		i32 offset;
		u8* ptr = nullptr;
		u32 size;
	} m_transient[2];
	u32 m_current_transient;

	JobSystem::SignalHandle m_transient_ready = JobSystem::INVALID_HANDLE;
	GPUProfiler m_profiler;

	struct MaterialBuffer {
		MaterialBuffer(IAllocator& alloc) : map(alloc), data(alloc) {}
		ffr::BufferHandle buffer = ffr::INVALID_BUFFER;
		Array<MaterialConsts> data;
		HashMap<u32, u32> map;
		u32 first_free = 1;
		// TODO this is not MT safe
		bool dirty = false;
	} m_material_buffer;
};


extern "C"
{
	LUMIX_PLUGIN_ENTRY(renderer)
	{
		return LUMIX_NEW(engine.getAllocator(), RendererImpl)(engine);
	}
}


} // namespace Lumix



