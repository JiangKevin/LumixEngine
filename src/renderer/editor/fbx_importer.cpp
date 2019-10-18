#include "fbx_importer.h"
#include "animation/animation.h"
#include "editor/asset_compiler.h"
#include "editor/studio_app.h"
#include "editor/world_editor.h"
#include "engine/crc32.h"
#include "engine/engine.h"
#include "engine/file_system.h"
#include "engine/log.h"
#include "engine/math.h"
#include "engine/os.h"
#include "engine/path_utils.h"
#include "engine/plugin_manager.h"
#include "engine/prefab.h"
#include "engine/profiler.h"
#include "engine/reflection.h"
#include "engine/resource_manager.h"
#include "engine/serializer.h"
#include "physics/physics_geometry.h"
#include "renderer/material.h"
#include "renderer/model.h"
#include "renderer/pipeline.h"
#include "renderer/renderer.h"
#include "renderer/shader.h"
#include <float.h>
#include <math.h>
#include <stdlib.h>


namespace Lumix
{


typedef StaticString<MAX_PATH_LENGTH> PathBuilder;



static void getMaterialName(const ofbx::Material* material, char (&out)[128])
{
	copyString(out, material ? material->name : "default");
	char* iter = out;
	while (*iter)
	{
		char c = *iter;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
		{
			*iter = '_';
		}
		++iter;
	}
	makeLowercase(Span(out), out);
}


void FBXImporter::getImportMeshName(const ImportMesh& mesh, char (&out)[256])
{
	const char* name = mesh.fbx->name;
	const ofbx::Material* material = mesh.fbx_mat;

	if (name[0] == '\0' && mesh.fbx->getParent()) name = mesh.fbx->getParent()->name;
	if (name[0] == '\0' && material) name = material->name;
	copyString(out, name);
	if(mesh.submesh >= 0) {
		catString(out, "_");
		char tmp[32];
		toCString(mesh.submesh, Span(tmp));
		catString(out, tmp);
	}
}


const FBXImporter::ImportMesh* FBXImporter::getAnyMeshFromBone(const ofbx::Object* node, int bone_idx) const
{
	for (int i = 0; i < meshes.size(); ++i)
	{
		const ofbx::Mesh* mesh = meshes[i].fbx;
		if (meshes[i].bone_idx == bone_idx) {
			return &meshes[i];
		}

		auto* skin = mesh->getGeometry()->getSkin();
		if (!skin) continue;

		for (int j = 0, c = skin->getClusterCount(); j < c; ++j)
		{
			if (skin->getCluster(j)->getLink() == node) return &meshes[i];
		}
	}
	return nullptr;
}


static ofbx::Matrix makeOFBXIdentity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }


static ofbx::Matrix getBindPoseMatrix(const FBXImporter::ImportMesh* mesh, const ofbx::Object* node)
{
	if (!mesh) return node->getGlobalTransform();
	if (!mesh->fbx) return makeOFBXIdentity();

	auto* skin = mesh->fbx->getGeometry()->getSkin();
	if (!skin) return node->getGlobalTransform();

	for (int i = 0, c = skin->getClusterCount(); i < c; ++i)
	{
		const ofbx::Cluster* cluster = skin->getCluster(i);
		if (cluster->getLink() == node)
		{
			return cluster->getTransformLinkMatrix();
		}
	}
	return node->getGlobalTransform();
}


void FBXImporter::gatherMaterials(const char* src_dir)
{
	for (ImportMesh& mesh : meshes)
	{
		const ofbx::Material* fbx_mat = mesh.fbx_mat;
		if (!fbx_mat) continue;

		ImportMaterial& mat = materials.emplace();
		mat.fbx = fbx_mat;

		auto gatherTexture = [&mat, src_dir](ofbx::Texture::TextureType type) {
			const ofbx::Texture* texture = mat.fbx->getTexture(type);
			if (!texture) return;

			ImportTexture& tex = mat.textures[type];
			tex.fbx = texture;
			ofbx::DataView filename = tex.fbx->getRelativeFileName();
			if (filename == "") filename = tex.fbx->getFileName();
			filename.toString(tex.path.data);
			tex.src = tex.path;
			tex.is_valid = OS::fileExists(tex.src);

			if (!tex.is_valid)
			{
				PathUtils::FileInfo file_info(tex.path);
				tex.src = src_dir;
				tex.src << file_info.m_basename << "." << file_info.m_extension;
				tex.is_valid = OS::fileExists(tex.src);

				if (!tex.is_valid)
				{
					tex.src = src_dir;
					tex.src << tex.path;
					tex.is_valid = OS::fileExists(tex.src);
				}
			}

			char tmp[MAX_PATH_LENGTH];
			PathUtils::normalize(tex.src, Span(tmp));
			tex.src = tmp;

			tex.import = true;
		};

		gatherTexture(ofbx::Texture::DIFFUSE);
		gatherTexture(ofbx::Texture::NORMAL);
		gatherTexture(ofbx::Texture::SPECULAR);
	}
}


void FBXImporter::insertHierarchy(Array<const ofbx::Object*>& bones, const ofbx::Object* node)
{
	if (!node) return;
	if (bones.indexOf(node) >= 0) return;
	ofbx::Object* parent = node->getParent();
	insertHierarchy(bones, parent);
	bones.push(node);
}


void FBXImporter::sortBones()
{
	int count = bones.size();
	for (int i = 0; i < count; ++i)
	{
		for (int j = i + 1; j < count; ++j)
		{
			if (bones[i]->getParent() == bones[j])
			{
				const ofbx::Object* bone = bones[j];
				bones.swapAndPop(j);
				bones.insert(i, bone);
				--i;
				break;
			}
		}
	}

	for (const ofbx::Object*& bone : bones) {
		const int idx = meshes.find([&](const ImportMesh& mesh){
			return mesh.fbx == bone;
		});

		if (idx >= 0) {
			meshes[idx].is_skinned = true;
			meshes[idx].bone_idx = int(&bone - bones.begin());
		}
	}
}


void FBXImporter::gatherBones(const ofbx::IScene& scene)
{
	for (const ImportMesh& mesh : meshes)
	{
		if(mesh.fbx->getGeometry()) {
			const ofbx::Skin* skin = mesh.fbx->getGeometry()->getSkin();
			if (skin)
			{
				for (int i = 0; i < skin->getClusterCount(); ++i)
				{
					const ofbx::Cluster* cluster = skin->getCluster(i);
					insertHierarchy(bones, cluster->getLink());
				}
			}
		}
	}

	for (int i = 0, n = scene.getAnimationStackCount(); i < n; ++i)
	{
		const ofbx::AnimationStack* stack = scene.getAnimationStack(i);
		for (int j = 0; stack->getLayer(j); ++j)
		{
			const ofbx::AnimationLayer* layer = stack->getLayer(j);
			for (int k = 0; layer->getCurveNode(k); ++k)
			{
				const ofbx::AnimationCurveNode* node = layer->getCurveNode(k);
				if (node->getBone()) insertHierarchy(bones, node->getBone());
			}
		}
	}

	bones.removeDuplicates();
	sortBones();
}


static void makeValidFilename(char* filename)
{
	char* c = filename;
	while (*c)
	{
		bool is_valid = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_';
		if (!is_valid) *c = '_';
		++c;
	}
}


void FBXImporter::gatherAnimations(const ofbx::IScene& scene)
{
	int anim_count = scene.getAnimationStackCount();
	for (int i = 0; i < anim_count; ++i)
	{
		ImportAnimation& anim = animations.emplace();
		anim.scene = &scene;
		anim.fbx = (const ofbx::AnimationStack*)scene.getAnimationStack(i);
		anim.import = true;
		const ofbx::TakeInfo* take_info = scene.getTakeInfo(anim.fbx->name);
		if (take_info)
		{
			if (take_info->name.begin != take_info->name.end)
			{
				take_info->name.toString(anim.name.data);
			}
			if (anim.name.empty() && take_info->filename.begin != take_info->filename.end)
			{
				char tmp[MAX_PATH_LENGTH];
				take_info->filename.toString(tmp);
				PathUtils::getBasename(Span(anim.name.data), tmp);
			}
			if (anim.name.empty()) anim.name << "anim";
		}
		else
		{
			anim.name = "anim";
		}
	}
}


static int findSubblobIndex(const OutputMemoryStream& haystack, const OutputMemoryStream& needle, const Array<int>& subblobs, int first_subblob)
{
	const u8* data = (const u8*)haystack.getData();
	const u8* needle_data = (const u8*)needle.getData();
	int step_size = (int)needle.getPos();
	int idx = first_subblob;
	while(idx != -1)
	{
		if (compareMemory(data + idx * step_size, needle_data, step_size) == 0) return idx;
		idx = subblobs[idx];
	}
	return -1;
}


static Vec3 toLumixVec3(const ofbx::Vec4& v) { return {(float)v.x, (float)v.y, (float)v.z}; }
static Vec3 toLumixVec3(const ofbx::Vec3& v) { return {(float)v.x, (float)v.y, (float)v.z}; }
static ofbx::Vec3 toOFBXVec3(const Vec3& v) { return {(float)v.x, (float)v.y, (float)v.z}; }
static Quat toLumix(const ofbx::Quat& q) { return {(float)q.x, (float)q.y, (float)q.z, (float)q.w}; }


static Matrix toLumix(const ofbx::Matrix& mtx)
{
	Matrix res;

	for (int i = 0; i < 16; ++i) (&res.m11)[i] = (float)mtx.m[i];

	return res;
}


static u32 packF4u(const Vec3& vec)
{
	const u8 xx = u8(vec.x * 127.0f + 128.0f);
	const u8 yy = u8(vec.y * 127.0f + 128.0f);
	const u8 zz = u8(vec.z * 127.0f + 128.0f);
	const u8 ww = u8(0);

	union {
		u32 ui32;
		u8 arr[4];
	} un;

	un.arr[0] = xx;
	un.arr[1] = yy;
	un.arr[2] = zz;
	un.arr[3] = ww;

	return un.ui32;
}


void FBXImporter::writePackedVec3(const ofbx::Vec3& vec, const Matrix& mtx, OutputMemoryStream* blob) const
{
	Vec3 v = toLumixVec3(vec);
	v = (mtx * Vec4(v, 0)).xyz();
	v.normalize();
	v = fixOrientation(v);

	u32 packed = packF4u(v);
	blob->write(packed);
}


static void writeUV(const ofbx::Vec2& uv, OutputMemoryStream* blob)
{
	Vec2 tex_cooords = {(float)uv.x, 1 - (float)uv.y};
	blob->write(tex_cooords);
}


static void writeColor(const ofbx::Vec4& color, OutputMemoryStream* blob)
{
	u8 rgba[4];
	rgba[0] = u8(color.x * 255);
	rgba[1] = u8(color.y * 255);
	rgba[2] = u8(color.z * 255);
	rgba[3] = u8(color.w * 255);
	blob->write(rgba);
}


static void writeSkin(const FBXImporter::Skin& skin, OutputMemoryStream* blob)
{
	blob->write(skin.joints);
	blob->write(skin.weights);
	float sum = skin.weights[0] + skin.weights[1] + skin.weights[2] + skin.weights[3];
	ASSERT(sum > 0.99f && sum < 1.01f);
}


static int getMaterialIndex(const ofbx::Mesh& mesh, const ofbx::Material& material)
{
	for (int i = 0, c = mesh.getMaterialCount(); i < c; ++i)
	{
		if (mesh.getMaterial(i) == &material) return i;
	}
	return -1;
}


static void centerMesh(const ofbx::Vec3* vertices, int vertices_count, FBXImporter::ImportConfig::Origin origin, Matrix* transform)
{
	if (vertices_count <= 0) return;

	ofbx::Vec3 min = vertices[0];
	ofbx::Vec3 max = vertices[0];

	for (int i = 1; i < vertices_count; ++i)
	{
		ofbx::Vec3 v = vertices[i];
			
		min.x = minimum(min.x, v.x);
		min.y = minimum(min.y, v.y);
		min.z = minimum(min.z, v.z);
			
		max.x = maximum(max.x, v.x);
		max.y = maximum(max.y, v.y);
		max.z = maximum(max.z, v.z);
	}

	Vec3 center;
	center.x = float(min.x + max.x) * 0.5f;
	center.y = float(min.y + max.y) * 0.5f;
	center.z = float(min.z + max.z) * 0.5f;
		
	if (origin == FBXImporter::ImportConfig::Origin::BOTTOM) center.y = (float)min.y;

	transform->setTranslation(-center);
}


static ofbx::Vec3 operator-(const ofbx::Vec3& a, const ofbx::Vec3& b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}


static ofbx::Vec2 operator-(const ofbx::Vec2& a, const ofbx::Vec2& b)
{
	return {a.x - b.x, a.y - b.y};
}


static void computeTangents(Array<ofbx::Vec3>& out, int vertex_count, const ofbx::Vec3* vertices, const ofbx::Vec3* normals, const ofbx::Vec2* uvs)
{
	/*out.resize(vertex_count);
	setMemory(out.begin(), 0, out.byte_size());
	for (int i = 0; i < vertex_count; i += 3) {
		const ofbx::Vec3 v0 = vertices[i + 0];
		const ofbx::Vec3 v1 = vertices[i + 1];
		const ofbx::Vec3 v2 = vertices[i + 2];
		const ofbx::Vec2 uv0 = uvs[0];
		const ofbx::Vec2 uv1 = uvs[1];
		const ofbx::Vec2 uv2 = uvs[2];

		const ofbx::Vec3 dv10 = v1 - v0;
		const ofbx::Vec3 dv20 = v2 - v0;
		const ofbx::Vec2 duv10 = uv1 - uv0;
		const ofbx::Vec2 duv20 = uv2 - uv0;

		const float dir = duv20.x * duv10.y - duv20.y * duv10.x < 0 ? -1.f : 1.f;
		ofbx::Vec3 tangent; 
		tagent.x = (dv20.x * duv10.y - dv10.x * duv2.y) * dir;
		tagent.y = (dv20.y * duv10.y - dv10.y * duv2.y) * dir;
		tagent.z = (dv20.z * duv10.y - dv10.z * duv2.y) * dir;
	}*/
}


void FBXImporter::postprocessMeshes(const ImportConfig& cfg)
{
	for (int mesh_idx = 0; mesh_idx < meshes.size(); ++mesh_idx)
	{
		ImportMesh& import_mesh = meshes[mesh_idx];
		import_mesh.vertex_data.clear();
		import_mesh.indices.clear();

		const ofbx::Mesh& mesh = *import_mesh.fbx;
		const ofbx::Geometry* geom = import_mesh.fbx->getGeometry();
		int vertex_count = geom->getVertexCount();
		const ofbx::Vec3* vertices = geom->getVertices();
		const ofbx::Vec3* normals = geom->getNormals();
		const ofbx::Vec3* tangents = geom->getTangents();
		const ofbx::Vec4* colors = import_vertex_colors ? geom->getColors() : nullptr;
		const ofbx::Vec2* uvs = geom->getUVs();

		Matrix transform_matrix = Matrix::IDENTITY;
		Matrix geometry_matrix = toLumix(mesh.getGeometricMatrix());
		transform_matrix = toLumix(mesh.getGlobalTransform()) * geometry_matrix;
		if (cancel_mesh_transforms) transform_matrix.setTranslation({0, 0, 0});
		if (cfg.origin != ImportConfig::Origin::SOURCE) {
			centerMesh(vertices, vertex_count, cfg.origin, &transform_matrix);
		}
		import_mesh.transform_matrix = transform_matrix;
		import_mesh.transform_matrix.inverse();

		OutputMemoryStream blob(allocator);
		int vertex_size = getVertexSize(import_mesh);
		import_mesh.vertex_data.reserve(vertex_count * vertex_size);

		Array<Skin> skinning(allocator);
		if (import_mesh.is_skinned) fillSkinInfo(skinning, import_mesh);

		AABB aabb = {{0, 0, 0}, {0, 0, 0}};
		float radius_squared = 0;

		int material_idx = getMaterialIndex(mesh, *import_mesh.fbx_mat);
		ASSERT(material_idx >= 0);

		int first_subblob[256];
		for (int& subblob : first_subblob) subblob = -1;
		Array<int> subblobs(allocator);
		subblobs.reserve(vertex_count);

		const int* materials = geom->getMaterials();
		Array<ofbx::Vec3> computed_tangents(allocator);
		if (!tangents && normals && uvs) {
			//computeTangents(computed_tangents, vertex_count, vertices, normals, uvs);
			//tangents = computed_tangents.begin();
		}

		for (int i = 0; i < vertex_count; ++i)
		{
			if (materials && materials[i / 3] != material_idx) continue;

			blob.clear();
			ofbx::Vec3 cp = vertices[i];
			// premultiply control points here, so we can have constantly-scaled meshes without scale in bones
			Vec3 pos = transform_matrix.transformPoint(toLumixVec3(cp)) * cfg.mesh_scale * fbx_scale;
			pos = fixOrientation(pos);
			blob.write(pos);

			float sq_len = pos.squaredLength();
			radius_squared = maximum(radius_squared, sq_len);

			aabb.min.x = minimum(aabb.min.x, pos.x);
			aabb.min.y = minimum(aabb.min.y, pos.y);
			aabb.min.z = minimum(aabb.min.z, pos.z);
			aabb.max.x = maximum(aabb.max.x, pos.x);
			aabb.max.y = maximum(aabb.max.y, pos.y);
			aabb.max.z = maximum(aabb.max.z, pos.z);

			if (normals) writePackedVec3(normals[i], transform_matrix, &blob);
			if (uvs) writeUV(uvs[i], &blob);
			if (colors) writeColor(colors[i], &blob);
			if (tangents) writePackedVec3(tangents[i], transform_matrix, &blob);
			if (import_mesh.is_skinned) writeSkin(skinning[i], &blob);

			u8 first_byte = ((const u8*)blob.getData())[0];

			int idx = findSubblobIndex(import_mesh.vertex_data, blob, subblobs, first_subblob[first_byte]);
			if (idx == -1)
			{
				subblobs.push(first_subblob[first_byte]);
				first_subblob[first_byte] = subblobs.size() - 1;
				import_mesh.indices.push((int)import_mesh.vertex_data.getPos() / vertex_size);
				import_mesh.vertex_data.write(blob.getData(), vertex_size);
			}
			else
			{
				import_mesh.indices.push(idx);
			}
		}

		import_mesh.aabb = aabb;
		import_mesh.radius_squared = radius_squared;
	}
	for (int mesh_idx = meshes.size() - 1; mesh_idx >= 0; --mesh_idx)
	{
		if (meshes[mesh_idx].indices.empty()) meshes.swapAndPop(mesh_idx);
	}
}


static int detectMeshLOD(const FBXImporter::ImportMesh& mesh)
{
	const char* node_name = mesh.fbx->name;
	const char* lod_str = stristr(node_name, "_LOD");
	if (!lod_str)
	{
		char mesh_name[256];
		FBXImporter::getImportMeshName(mesh, mesh_name);
		if (!mesh_name) return 0;

		const char* lod_str = stristr(mesh_name, "_LOD");
		if (!lod_str) return 0;
	}

	lod_str += stringLength("_LOD");

	int lod;
	fromCString(Span(lod_str, stringLength(lod_str)), Ref(lod));

	return lod;
}


void FBXImporter::gatherMeshes(ofbx::IScene* scene)
{
	int min_lod = 2;
	int c = scene->getMeshCount();
	int start_index = meshes.size();
	for (int i = 0; i < c; ++i)
	{
		const ofbx::Mesh* fbx_mesh = (const ofbx::Mesh*)scene->getMesh(i);
		//if (fbx_mesh->getGeometry()->getVertexCount() == 0) continue;
		const int mat_count = fbx_mesh->getMaterialCount();
		for (int j = 0; j < mat_count; ++j)
		{
			ImportMesh& mesh = meshes.emplace(allocator);
			mesh.is_skinned = !ignore_skeleton && fbx_mesh->getGeometry() && fbx_mesh->getGeometry()->getSkin();
			mesh.fbx = fbx_mesh;
			mesh.fbx_mat = fbx_mesh->getMaterial(j);
			mesh.submesh = mat_count > 1 ? j : -1;
			mesh.lod = detectMeshLOD(mesh);
			min_lod = minimum(min_lod, mesh.lod);
		}
	}
	if (min_lod != 1) return;
	for (int i = start_index, n = meshes.size(); i < n; ++i)
	{
		--meshes[i].lod;
	}
}


FBXImporter::~FBXImporter()
{
	if (scene) scene->destroy();
}


FBXImporter::FBXImporter(StudioApp& app)
	: allocator(app.getWorldEditor().getAllocator())
	, compiler(app.getAssetCompiler())
	, scene(nullptr)
	, materials(allocator)
	, meshes(allocator)
	, animations(allocator)
	, bones(allocator)
	, out_file(allocator)
	, filesystem(app.getWorldEditor().getEngine().getFileSystem())
	, app(app)
{
	out_file.reserve(1024 * 1024);
}


bool FBXImporter::setSource(const char* filename, bool ignore_geometry)
{
	PROFILE_FUNCTION();
	if(scene) {
		scene->destroy();
		scene = nullptr;	
		meshes.clear();
		materials.clear();
		animations.clear();
		bones.clear();
	}

	Array<u8> data(allocator);
	if (!filesystem.getContentSync(Path(filename), Ref(data))) return false;
	
	const u64 flags = ignore_geometry ? (u64)ofbx::LoadFlags::IGNORE_GEOMETRY : (u64)ofbx::LoadFlags::TRIANGULATE;
	scene = ofbx::load(&data[0], data.size(), flags);
	if (!scene)
	{
		logError("FBX") << "Failed to import \"" << filename << ": " << ofbx::getError();
		return false;
	}
	fbx_scale = scene->getGlobalSettings()->UnitScaleFactor * 0.01f;

	const ofbx::GlobalSettings* settings = scene->getGlobalSettings();
	switch (settings->UpAxis) {
		case ofbx::UpVector_AxisX: orientation = Orientation::X_UP; break;
		case ofbx::UpVector_AxisY: orientation = Orientation::Y_UP; break;
		case ofbx::UpVector_AxisZ: orientation = Orientation::Z_UP; break;
	}
	root_orientation = orientation;

	char src_dir[MAX_PATH_LENGTH];
	PathUtils::getDir(Span(src_dir), filename);
	gatherMeshes(scene);

	gatherAnimations(*scene);
	if (!ignore_geometry) {
		gatherMaterials(src_dir);
		materials.removeDuplicates([](const ImportMaterial& a, const ImportMaterial& b) { return a.fbx == b.fbx; });
		gatherBones(*scene);
	}

	return true;
}


void FBXImporter::writeString(const char* str) { out_file.write(str, stringLength(str)); }

static Vec3 impostorToWorld(Vec2 uv) {
	Vec3 position = Vec3(
						0.0f + (uv.x - uv.y),
						-1.0f + (uv.x + uv.y),
						0.0f
					);

	Vec2 absolute;
	absolute.x = abs(position.x);
	absolute.y = abs(position.y);
	position.z = 1.0f - absolute.x - absolute.y;

	return Vec3{position.x, position.z, position.y};
};

static constexpr u32 IMPOSTOR_TILE_SIZE = 512;
static constexpr u32 IMPOSTOR_COLS = 9;

static void getBBProjection(const AABB& aabb, Ref<Vec2> out_min, Ref<Vec2> out_max) {
	const float radius = (aabb.max - aabb.min).length() * 0.5f;
	const Vec3 center = (aabb.min + aabb.max) * 0.5f;

	Matrix proj;
	proj.setOrtho(-1, 1, -1, 1, 0, radius * 2, false, true);
	Vec2 min(FLT_MAX, FLT_MAX), max(-FLT_MAX, -FLT_MAX);
	for (u32 j = 0; j < IMPOSTOR_COLS; ++j) {
		for (u32 i = 0; i < IMPOSTOR_COLS; ++i) {
			const Vec3 v = impostorToWorld({i / (float)(IMPOSTOR_COLS - 1), j / (float)(IMPOSTOR_COLS - 1)});
			Matrix view;
			view.lookAt(center + v, center, Vec3(0, 1, 0));
			const Matrix vp = proj * view;
			for (u32 k = 0; k < 8; ++k) {
				const Vec3 p = {
					k & 1 ? aabb.min.x : aabb.max.x,
					k & 2 ? aabb.min.y : aabb.max.y,
					k & 4 ? aabb.min.z : aabb.max.z
				};
				const Vec4 proj_p = vp * Vec4(p, 1);
				min.x = minimum(min.x, proj_p.x / proj_p.w);
				min.y = minimum(min.y, proj_p.y / proj_p.w);
				max.x = maximum(max.x, proj_p.x / proj_p.w);
				max.y = maximum(max.y, proj_p.y / proj_p.w);
			}
		}
	}
	out_min = min;
	out_max = max;
}

struct CaptureImpostorJob : Renderer::RenderJob {

	CaptureImpostorJob(Ref<Array<u32>> gb0, Ref<Array<u32>> gb1, Ref<IVec2> size) 
		: m_gb0(gb0)
		, m_gb1(gb1)
		, m_tile_size(size)
	{}

	void setup() override {}

	void execute() override {
		ffr::TextureHandle gbs[] = { ffr::allocTextureHandle(), ffr::allocTextureHandle(), ffr::allocTextureHandle() };

		ffr::BufferHandle pass_buf = ffr::allocBufferHandle();
		ffr::BufferHandle ub = ffr::allocBufferHandle();
		ffr::createBuffer(ub, (u32)ffr::BufferFlags::UNIFORM_BUFFER, 256, nullptr);
		const u32 pass_buf_size = (sizeof(PassState) + 255) & ~255;
		ffr::createBuffer(pass_buf, (u32)ffr::BufferFlags::UNIFORM_BUFFER, pass_buf_size, nullptr);
		ffr::bindUniformBuffer(1, pass_buf, 0, pass_buf_size);
		ffr::bindUniformBuffer(4, ub, 0, 256);

		const AABB aabb = m_model->getAABB();
		const Vec3 center = (aabb.min + aabb.max) * 0.5f;
		const float radius = m_model->getBoundingRadius();
		Vec2 min, max;
		getBBProjection(aabb, Ref(min), Ref(max));
		const Vec2 size = max - min;

		m_tile_size = IVec2(int(IMPOSTOR_TILE_SIZE * size.x / size.y), IMPOSTOR_TILE_SIZE);
		m_tile_size->x = (m_tile_size->x + 3) & ~3;
		m_tile_size->y = (m_tile_size->y + 3) & ~3;
		const IVec2 texture_size = m_tile_size.value * IMPOSTOR_COLS;
		ffr::createTexture(gbs[0], texture_size.x, texture_size.y, 1, ffr::TextureFormat::RGBA8, (u32)ffr::TextureFlags::NO_MIPS, nullptr, "impostor_gb0");
		ffr::createTexture(gbs[1], texture_size.x, texture_size.y, 1, ffr::TextureFormat::RGBA8, (u32)ffr::TextureFlags::NO_MIPS, nullptr, "impostor_gb1");
		ffr::createTexture(gbs[2], texture_size.x, texture_size.y, 1, ffr::TextureFormat::D24S8, (u32)ffr::TextureFlags::NO_MIPS, nullptr, "impostor_gbd");
		
		ffr::setFramebuffer(gbs, 3, 0);
		const float color[] = {0, 0, 0, 0};
		ffr::clear((u32)ffr::ClearFlags::COLOR | (u32)ffr::ClearFlags::DEPTH | (u32)ffr::ClearFlags::STENCIL, color, 0);

		for (u32 j = 0; j < IMPOSTOR_COLS; ++j) {
			for (u32 i = 0; i < IMPOSTOR_COLS; ++i) {
				ffr::viewport(i * m_tile_size->x, j * m_tile_size->y, m_tile_size->x, m_tile_size->y);
				const u32 mesh_count = m_model->getMeshCount();
				;
				for (u32 k = 0; k <= (u32)m_model->getLODs()[0].to_mesh; ++k) {
					const Mesh& mesh = m_model->getMesh(k);

					const Material* material = mesh.material;
					const Mesh::RenderData* rd = mesh.render_data;

					ShaderRenderData* shader_rd = material->getShader()->m_render_data;
					const ffr::ProgramHandle prog = Shader::getProgram(shader_rd, rd->vertex_decl, m_capture_define | material->getDefineMask());

					if(!prog.isValid()) continue;

					const Material::RenderData* mat_rd = material->getRenderData();
					ffr::bindTextures(mat_rd->textures, 0, mat_rd->textures_count);

					const Vec3 v = impostorToWorld({i / (float)(IMPOSTOR_COLS - 1), j / (float)(IMPOSTOR_COLS - 1)});

					Matrix model_mtx;
					if (i == IMPOSTOR_COLS >> 1 && j == IMPOSTOR_COLS >> 1) {
						model_mtx.lookAt(Vec3(0, 0, 0), v, Vec3(0, 0, 1));
					}
					else {
						model_mtx.lookAt(Vec3(0, 0, 0), v, Vec3(0, 1, 0));
					}
					ffr::update(ub, &model_mtx.m11, sizeof(model_mtx));
					PassState pass_state;
					pass_state.view.lookAt(center + Vec3(0, 0, 2 * radius), center, {0, 1, 0});
					pass_state.projection.setOrtho(min.x, max.x, min.y, max.y, 0, 5 * radius, false, true);
					pass_state.inv_projection = pass_state.projection.inverted();
					pass_state.inv_view = pass_state.view.fastInverted();
					pass_state.view_projection = pass_state.projection * pass_state.view;
					pass_state.inv_view_projection = pass_state.view_projection.inverted();
					pass_state.view_dir = Vec4(pass_state.view.inverted().transformVector(Vec3(0, 0, -1)), 0);

					ffr::update(pass_buf, &pass_state, sizeof(pass_state));
					ffr::useProgram(prog);
					ffr::bindIndexBuffer(rd->index_buffer_handle);
					ffr::bindVertexBuffer(0, rd->vertex_buffer_handle, 0, rd->vb_stride);
					ffr::bindVertexBuffer(1, ffr::INVALID_BUFFER, 0, 0);
					ffr::setState(u64(ffr::StateFlags::DEPTH_TEST) | u64(ffr::StateFlags::DEPTH_WRITE) | material->getRenderStates());
					ffr::drawTriangles(rd->indices_count, rd->index_type);
				}
			}
		}

		ffr::setFramebuffer(nullptr, 0, 0);

		m_gb0->resize(texture_size.x * texture_size.y);
		m_gb1->resize(m_gb0->size());
		ffr::getTextureImage(gbs[0], m_gb0->byte_size(), m_gb0->begin());
		ffr::getTextureImage(gbs[1], m_gb1->byte_size(), m_gb1->begin());

		ffr::destroy(ub);
		ffr::destroy(gbs[0]);
		ffr::destroy(gbs[1]);
		ffr::destroy(gbs[2]);
	}

	Ref<Array<u32>> m_gb0;
	Ref<Array<u32>> m_gb1;
	Model* m_model;
	u32 m_capture_define;
	Ref<IVec2> m_tile_size;
};

bool FBXImporter::createImpostorTextures(Model* model, Ref<Array<u32>> gb0_rgba, Ref<Array<u32>> gb1_rgba, Ref<IVec2> size)
{
	ASSERT(model->isReady());

	Engine& engine = app.getWorldEditor().getEngine();
	Renderer* renderer = (Renderer*)engine.getPluginManager().getPlugin("renderer");
	ASSERT(renderer);

	IAllocator& allocator = renderer->getAllocator();
	CaptureImpostorJob* job = LUMIX_NEW(allocator, CaptureImpostorJob)(gb0_rgba, gb1_rgba, size);
	// TODO do not use m_model in render thread
	job->m_model = model;
	job->m_capture_define = 1 << renderer->getShaderDefineIdx("DEFERRED");
	renderer->queue(job, 0);
	renderer->frame();
	renderer->frame();
	
	return true;
}


void FBXImporter::writeMaterials(const char* src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION()
	const PathUtils::FileInfo src_info(src);
	for (const ImportMaterial& material : materials) {
		if (!material.import) continue;

		char mat_name[128];
		getMaterialName(material.fbx, mat_name);

		const StaticString<MAX_PATH_LENGTH + 128> mat_src(src_info.m_dir, mat_name, ".mat");
		if (filesystem.fileExists(mat_src)) continue;

		OS::OutputFile f;
		if (!filesystem.open(mat_src, Ref(f)))
		{
			logError("FBX") << "Failed to create " << mat_src;
			continue;
		}
		out_file.clear();

		writeString("shader \"pipelines/standard.shd\"\n");
		if (material.alpha_cutout) writeString("defines {\"ALPHA_CUTOUT\"}\n");
		auto writeTexture = [this](const ImportTexture& texture, u32 idx) {
			if (texture.is_valid && idx < 2) {
				PathUtils::FileInfo info(texture.src);
				const StaticString<MAX_PATH_LENGTH> meta_path(info.m_dir, info.m_basename, ".meta");
				if (!OS::fileExists(meta_path)) {
					OS::OutputFile file;
					if (file.open(meta_path)) {
						
						file << (idx == 0 ? "srgb = true\n" : "normalmap = true\n");
						file.close();
					}
				}
			}
			if (texture.fbx)
			{
				writeString("texture \"/");
				writeString(texture.src);
				writeString("\"\n");
			}
			else
			{
				writeString("texture \"\"\n");
			}
		};

		writeTexture(material.textures[0], 0);
		writeTexture(material.textures[1], 1);
		writeTexture(material.textures[2], 2);
		
/*			ofbx::Color diffuse_color = material.fbx->getDiffuseColor();
		out_file << "color {" << diffuse_color.r 
			<< "," << diffuse_color.g
			<< "," << diffuse_color.b
			<< ",1}\n";*/

		if (!f.write(out_file.getData(), out_file.getPos())) {
			logError("FBX") << "Failed to write " << mat_src;
		}
		f.close();
	}

	if (cfg.create_impostor) {
		const StaticString<MAX_PATH_LENGTH> mat_src(src_info.m_dir, src_info.m_basename, "_impostor.mat");
		if (!filesystem.fileExists(mat_src)) {
			OS::OutputFile f;
			if (!filesystem.open(mat_src, Ref(f))) {
				logError("FBX") << "Failed to create " << mat_src;
			}
			else {
				f << "shader \"/pipelines/standard.shd\"\n";
				f.close();
			}
		}
	}
}


static Vec3 getTranslation(const ofbx::Matrix& mtx)
{
	return {(float)mtx.m[12], (float)mtx.m[13], (float)mtx.m[14]};
}


static Quat getRotation(const ofbx::Matrix& mtx)
{
	Matrix m = toLumix(mtx);
	m.normalizeScale();
	return m.getRotation();
}

static void fill(Array<FBXImporter::RotationKey>& out, const ofbx::AnimationCurveNode* curve_node, const ofbx::Object& bone) {
	ASSERT(out.empty());
	
	const ofbx::AnimationCurve* x_curve = curve_node->getCurve(0);
	const ofbx::AnimationCurve* y_curve = curve_node->getCurve(1);
	const ofbx::AnimationCurve* z_curve = curve_node->getCurve(2);

	if (x_curve) out.resize(x_curve->getKeyCount());
	else if (y_curve) out.resize(y_curve->getKeyCount());
	else if (z_curve) out.resize(z_curve->getKeyCount());
	else return;

	setMemory(out.begin(), 0, out.byte_size());

	auto fill_curve = [&](int idx, const ofbx::AnimationCurve* curve){
		if (!curve) {
			// TODO default value
			//ASSERT(false);
			return;
		}

		// we do not support nodes with curves with different number of keyframes
		ASSERT(curve->getKeyCount() == out.size());

		const i64* times = curve->getKeyTime();
		const float* values = curve->getKeyValue();
		for (u32 i = 0, c = curve->getKeyCount(); i < c; ++i) {
			(&out[i].rot.x)[idx] = values[i];
			// node with two curves with different times, this is not supported
			ASSERT(out[i].time == 0 || out[i].time == times[i]);
			out[i].time = times[i];
		}
	};

	fill_curve(0, x_curve);
	fill_curve(1, y_curve);
	fill_curve(2, z_curve);

	ofbx::Vec3 lcl_translation = bone.getLocalTranslation();
	for (FBXImporter::RotationKey& key : out) {
		key.rot = getRotation(bone.evalLocal(lcl_translation, {key.rot.x, key.rot.y, key.rot.z}));
	}
}

static void fill(Array<FBXImporter::TranslationKey>& out, const ofbx::AnimationCurveNode* curve_node, const ofbx::Object& bone, float parent_scale) {
	ASSERT(out.empty());
	
	const ofbx::AnimationCurve* x_curve = curve_node->getCurve(0);
	const ofbx::AnimationCurve* y_curve = curve_node->getCurve(1);
	const ofbx::AnimationCurve* z_curve = curve_node->getCurve(2);

	if (x_curve) out.resize(x_curve->getKeyCount());
	else if (y_curve) out.resize(y_curve->getKeyCount());
	else if (z_curve) out.resize(z_curve->getKeyCount());
	else return;

	setMemory(out.begin(), 0, out.byte_size());

	auto fill_curve = [&](int idx, const ofbx::AnimationCurve* curve){
		if (!curve) {
			// TODO default value
			//ASSERT(false);
			return;
		}

		// we do not support nodes with curves with different number of keyframes
		ASSERT(curve->getKeyCount() == out.size());

		const i64* times = curve->getKeyTime();
		const float* values = curve->getKeyValue();
		for (u32 i = 0, c = curve->getKeyCount(); i < c; ++i) {
			(&out[i].pos.x)[idx] = values[i];
			// node with two curves with different times, this is not supported
			ASSERT(out[i].time == 0 || out[i].time == times[i]);
			out[i].time = times[i];
		}
	};

	fill_curve(0, x_curve);
	fill_curve(1, y_curve);
	fill_curve(2, z_curve);

	const ofbx::Vec3 lcl_rotation = bone.getLocalRotation();
	for (FBXImporter::TranslationKey& key : out) {
		key.pos = getTranslation(bone.evalLocal(toOFBXVec3(key.pos), lcl_rotation))  * parent_scale;
	}
}


// arg parent_scale - animated scale is not supported, but we can get rid of static scale if we ignore
// it in writeSkeleton() and use parent_scale in this function
static void compressPositions(Array<FBXImporter::TranslationKey>& out,
	const ofbx::AnimationCurveNode* curve_node,
	const ofbx::Object& bone,
	float error,
	float parent_scale,
	double anim_len)
{
	out.clear();
	if (!curve_node) return;

	fill(out, curve_node, bone, parent_scale);
	if (out.empty()) return;

	FBXImporter::TranslationKey prev = out[0];
	if (out.size() == 1) {
		out.push(out[0]);
		out[1].time = ofbx::secondsToFbxTime(anim_len);
	}
	Vec3 dir = out[1].pos - out[0].pos;
	dir *= float(1 / ofbx::fbxTimeToSeconds(out[1].time - out[0].time));
	for (u32 i = 2; i < (u32)out.size(); ++i) {
		const Vec3 estimate = prev.pos + dir * (float)ofbx::fbxTimeToSeconds(out[i].time - prev.time);
		if (fabs(estimate.x - out[i].pos.x) > error || fabs(estimate.y - out[i].pos.y) > error ||
			fabs(estimate.z - out[i].pos.z) > error) 
		{
			prev = out[i - 1];
			dir = out[i].pos - out[i - 1].pos;
			dir *= float(1 / ofbx::fbxTimeToSeconds(out[i].time - out[i - 1].time));
		}
		else {
			out.erase(i - 1);
			--i;
		}
	}
}


static void compressRotations(Array<FBXImporter::RotationKey>& out,
	const ofbx::AnimationCurveNode* curve_node,
	const ofbx::Object& bone,
	float error,
	double anim_len)
{
	out.clear();
	if (!curve_node) return;

	fill(out, curve_node, bone);
	if (out.empty()) return;

	FBXImporter::RotationKey prev = out[0];
	if (out.size() == 1) {
		out.push(out[0]);
		out[1].time = ofbx::secondsToFbxTime(anim_len);
	}
	FBXImporter::RotationKey after_prev = out[1];
	for (u32 i = 2; i < (u32)out.size(); ++i) {
		const float t = float(ofbx::fbxTimeToSeconds(after_prev.time - prev.time) / ofbx::fbxTimeToSeconds(out[i].time - prev.time));
		const Quat estimate = nlerp(prev.rot, out[i].rot, t);
		if (fabs(estimate.x - after_prev.rot.x) > error || fabs(estimate.y - after_prev.rot.y) > error ||
			fabs(estimate.z - after_prev.rot.z) > error) 
		{
			prev = out[i - 1];
			after_prev = out[i];
		}
		else {
			out.erase(i - 1);
			--i;
		}
	}
}


static float getScaleX(const ofbx::Matrix& mtx)
{
	Vec3 v(float(mtx.m[0]), float(mtx.m[4]), float(mtx.m[8]));

	return v.length();
}


static int getDepth(const ofbx::Object* bone)
{
	int depth = 0;
	while (bone)
	{
		++depth;
		bone = bone->getParent();
	}
	return depth;
}


void FBXImporter::writeAnimations(const char* src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION();
	for (const FBXImporter::ImportAnimation& anim : getAnimations()) { 
		ASSERT(anim.import);

		const ofbx::AnimationStack* stack = anim.fbx;
		const ofbx::IScene& scene = *anim.scene;
		const ofbx::TakeInfo* take_info = scene.getTakeInfo(stack->name);
		if(!take_info && startsWith(stack->name, "AnimStack::")) {
			take_info = scene.getTakeInfo(stack->name + 11);
		}

		double anim_len;
		if (take_info) {
			anim_len = take_info->local_time_to - take_info->local_time_from;
		}
		else if(scene.getGlobalSettings()) {
			anim_len = scene.getGlobalSettings()->TimeSpanStop;
		}
		else {
			logError("Renderer") << "Unsupported animation in " << src;
			continue;
		}

		out_file.clear();

		Animation::Header header;
		header.magic = Animation::HEADER_MAGIC;
		header.version = 3;
		header.length = Time::fromSeconds((float)anim_len);
		write(header);

		write(anim.root_motion_bone_idx);
		int used_bone_count = 0;

		for (const ofbx::Object* bone : bones) {
			if (&bone->getScene() != &scene) continue;

			const ofbx::AnimationLayer* layer = stack->getLayer(0);
			const ofbx::AnimationCurveNode* translation_curve_node = layer->getCurveNode(*bone, "Lcl Translation");
			const ofbx::AnimationCurveNode* rotation_curve_node = layer->getCurveNode(*bone, "Lcl Rotation");
			if (translation_curve_node || rotation_curve_node) ++used_bone_count;
		}

		write(used_bone_count);
		Array<TranslationKey> positions(allocator);
		Array<RotationKey> rotations(allocator);
		auto fbx_to_anim_time = [anim_len](i64 fbx_time){
			const double t = clamp(ofbx::fbxTimeToSeconds(fbx_time) / anim_len, 0.0, 1.0);
			return u16(t * 0xffFF);
		};

		for (const ofbx::Object* bone : bones) {
			if (&bone->getScene() != &scene) continue;
			const ofbx::Object* root_bone = anim.root_motion_bone_idx >= 0 ? bones[anim.root_motion_bone_idx] : nullptr;

			const ofbx::AnimationLayer* layer = stack->getLayer(0);
			const ofbx::AnimationCurveNode* translation_node = layer->getCurveNode(*bone, "Lcl Translation");
			const ofbx::AnimationCurveNode* rotation_node = layer->getCurveNode(*bone, "Lcl Rotation");
			if (!translation_node && !rotation_node) continue;

			u32 name_hash = crc32(bone->name);
			write(name_hash);

			int depth = getDepth(bone);
			float parent_scale = bone->getParent() ? (float)getScaleX(bone->getParent()->getGlobalTransform()) : 1;
			compressPositions(positions, translation_node, *bone, position_error / depth, parent_scale, anim_len);
			write(positions.size());

			for (TranslationKey& key : positions) write(fbx_to_anim_time(key.time));
			for (TranslationKey& key : positions) {
				if (bone == root_bone) {
					write(fixRootOrientation(key.pos * cfg.mesh_scale * fbx_scale));
				}
				else {
					write(fixOrientation(key.pos * cfg.mesh_scale * fbx_scale));
				}
			}

			compressRotations(rotations, rotation_node, *bone, rotation_error / depth, anim_len);

			write(rotations.size());
			for (RotationKey& key : rotations) write(fbx_to_anim_time(key.time));
			for (RotationKey& key : rotations) {
				if (bone == root_bone) {
					write(fixRootOrientation(key.rot));
				}
				else {
					write(fixOrientation(key.rot));
				}
			}
		}

		const StaticString<MAX_PATH_LENGTH> anim_path(anim.name, ".ani:", src);
		compiler.writeCompiledResource(anim_path, Span((u8*)out_file.getData(), (i32)out_file.getPos()));
	}
}

int FBXImporter::getVertexSize(const ImportMesh& mesh) const
{
	static const int POSITION_SIZE = sizeof(float) * 3;
	static const int NORMAL_SIZE = sizeof(u8) * 4;
	static const int TANGENT_SIZE = sizeof(u8) * 4;
	static const int UV_SIZE = sizeof(float) * 2;
	static const int COLOR_SIZE = sizeof(u8) * 4;
	static const int BONE_INDICES_WEIGHTS_SIZE = sizeof(float) * 4 + sizeof(u16) * 4;
	int size = POSITION_SIZE;

	if (mesh.fbx->getGeometry()->getNormals()) size += NORMAL_SIZE;
	if (mesh.fbx->getGeometry()->getUVs()) size += UV_SIZE;
	if (mesh.fbx->getGeometry()->getColors() && import_vertex_colors) size += COLOR_SIZE;
	if (mesh.fbx->getGeometry()->getTangents()) size += TANGENT_SIZE;
	if (mesh.is_skinned) size += BONE_INDICES_WEIGHTS_SIZE;

	return size;
}


void FBXImporter::fillSkinInfo(Array<Skin>& skinning, const ImportMesh& import_mesh) const
{
	const ofbx::Mesh* mesh = import_mesh.fbx;
	const ofbx::Geometry* geom = mesh->getGeometry();
	skinning.resize(geom->getVertexCount());
	setMemory(&skinning[0], 0, skinning.size() * sizeof(skinning[0]));

	auto* skin = mesh->getGeometry()->getSkin();
	if(!skin) {
		ASSERT(import_mesh.bone_idx >= 0);
		skinning.resize(mesh->getGeometry()->getIndexCount());
		for (Skin& skin : skinning) {
			skin.count = 1;
			skin.weights[0] = 1;
			skin.weights[1] = skin.weights[2] = skin.weights[3] = 0;
			skin.joints[0] = skin.joints[1] = skin.joints[2] = skin.joints[3] = import_mesh.bone_idx;
		}
		return;
	}

	for (int i = 0, c = skin->getClusterCount(); i < c; ++i)
	{
		const ofbx::Cluster* cluster = skin->getCluster(i);
		if (cluster->getIndicesCount() == 0) continue;
		int joint = bones.indexOf(cluster->getLink());
		ASSERT(joint >= 0);
		const int* cp_indices = cluster->getIndices();
		const double* weights = cluster->getWeights();
		for (int j = 0; j < cluster->getIndicesCount(); ++j)
		{
			int idx = cp_indices[j];
			float weight = (float)weights[j];
			Skin& s = skinning[idx];
			if (s.count < 4)
			{
				s.weights[s.count] = weight;
				s.joints[s.count] = joint;
				++s.count;
			}
			else
			{
				int min = 0;
				for (int m = 1; m < 4; ++m)
				{
					if (s.weights[m] < s.weights[min]) min = m;
				}

				if (s.weights[min] < weight)
				{
					s.weights[min] = weight;
					s.joints[min] = joint;
				}
			}
		}
	}

	for (Skin& s : skinning)
	{
		float sum = 0;
		for (float w : s.weights) sum += w;
		for (float& w : s.weights) w /= sum;
	}
}


Vec3 FBXImporter::fixRootOrientation(const Vec3& v) const
{
	switch (root_orientation)
	{
		case Orientation::Y_UP: return Vec3(v.x, v.y, v.z);
		case Orientation::Z_UP: return Vec3(v.x, v.z, -v.y);
		case Orientation::Z_MINUS_UP: return Vec3(v.x, -v.z, v.y);
		case Orientation::X_MINUS_UP: return Vec3(v.y, -v.x, v.z);
		case Orientation::X_UP: return Vec3(-v.y, v.x, v.z);
	}
	ASSERT(false);
	return Vec3(v.x, v.y, v.z);
}


Quat FBXImporter::fixRootOrientation(const Quat& v) const
{
	switch (root_orientation)
	{
		case Orientation::Y_UP: return Quat(v.x, v.y, v.z, v.w);
		case Orientation::Z_UP: return Quat(v.x, v.z, -v.y, v.w);
		case Orientation::Z_MINUS_UP: return Quat(v.x, -v.z, v.y, v.w);
		case Orientation::X_MINUS_UP: return Quat(v.y, -v.x, v.z, v.w);
		case Orientation::X_UP: return Quat(-v.y, v.x, v.z, v.w);
	}
	ASSERT(false);
	return Quat(v.x, v.y, v.z, v.w);
}


Vec3 FBXImporter::fixOrientation(const Vec3& v) const
{
	switch (orientation)
	{
		case Orientation::Y_UP: return Vec3(v.x, v.y, v.z);
		case Orientation::Z_UP: return Vec3(v.x, v.z, -v.y);
		case Orientation::Z_MINUS_UP: return Vec3(v.x, -v.z, v.y);
		case Orientation::X_MINUS_UP: return Vec3(v.y, -v.x, v.z);
		case Orientation::X_UP: return Vec3(-v.y, v.x, v.z);
	}
	ASSERT(false);
	return Vec3(v.x, v.y, v.z);
}


Quat FBXImporter::fixOrientation(const Quat& v) const
{
	switch (orientation)
	{
		case Orientation::Y_UP: return Quat(v.x, v.y, v.z, v.w);
		case Orientation::Z_UP: return Quat(v.x, v.z, -v.y, v.w);
		case Orientation::Z_MINUS_UP: return Quat(v.x, -v.z, v.y, v.w);
		case Orientation::X_MINUS_UP: return Quat(v.y, -v.x, v.z, v.w);
		case Orientation::X_UP: return Quat(-v.y, v.x, v.z, v.w);
	}
	ASSERT(false);
	return Quat(v.x, v.y, v.z, v.w);
}

void FBXImporter::writeImpostorVertices(const AABB& aabb)
{
	#pragma pack(1)
		struct Vertex
		{
			Vec3 pos;
			u8 normal[4];
			u8 tangent[4];
			Vec2 uv;
		};
	#pragma pack()

	const float radius = (aabb.max - aabb.min).length() * 0.5f;
	const Vec3 center = (aabb.max + aabb.min) * 0.5f;

	Vec2 min, max;
	getBBProjection(aabb, Ref(min), Ref(max));

	const Vertex vertices[] = {
		{{center.x + min.x, center.y + min.y, center.z},	{128, 255, 128, 0},	 {255, 128, 128, 0}, {0, 0}},
		{{center.x + min.x, center.y + max.y, center.z},	{128, 255, 128, 0},	 {255, 128, 128, 0}, {0, 1}},
		{{center.x + max.x, center.y + max.y, center.z},	{128, 255, 128, 0},	 {255, 128, 128, 0}, {1, 1}},
		{{center.x + max.x, center.y + min.y, center.z},	{128, 255, 128, 0},	 {255, 128, 128, 0}, {1, 0}}
	};

	const u32 vertex_data_size = sizeof(vertices);
	write(vertex_data_size);
	for (const Vertex& vertex : vertices) {
		write(vertex.pos);
		write(vertex.normal);
		write(vertex.tangent);
		write(vertex.uv);
	}
}


void FBXImporter::writeGeometry(int mesh_idx)
{
	AABB aabb = {{0, 0, 0}, {0, 0, 0}};
	float radius_squared = 0;
	OutputMemoryStream vertices_blob(allocator);
	const ImportMesh& import_mesh = meshes[mesh_idx];
	
	bool are_indices_16_bit = areIndices16Bit(import_mesh);
	if (are_indices_16_bit)
	{
		int index_size = sizeof(u16);
		write(index_size);
		write(import_mesh.indices.size());
		for (int i : import_mesh.indices)
		{
			ASSERT(i <= (1 << 16));
			u16 index = (u16)i;
			write(index);
		}
	}
	else
	{
		int index_size = sizeof(import_mesh.indices[0]);
		write(index_size);
		write(import_mesh.indices.size());
		write(&import_mesh.indices[0], sizeof(import_mesh.indices[0]) * import_mesh.indices.size());
	}
	aabb.merge(import_mesh.aabb);
	radius_squared = maximum(radius_squared, import_mesh.radius_squared);

	write((i32)import_mesh.vertex_data.getPos());
	write(import_mesh.vertex_data.getData(), import_mesh.vertex_data.getPos());

	write(sqrtf(radius_squared) * bounding_shape_scale);
	aabb.min *= bounding_shape_scale;
	aabb.max *= bounding_shape_scale;
	write(aabb);
}


void FBXImporter::writeGeometry(const ImportConfig& cfg)
{
	AABB aabb = {{0, 0, 0}, {0, 0, 0}};
	float radius_squared = 0;
	OutputMemoryStream vertices_blob(allocator);
	for (const ImportMesh& import_mesh : meshes)
	{
		if (!import_mesh.import) continue;
		bool are_indices_16_bit = areIndices16Bit(import_mesh);
		if (are_indices_16_bit)
		{
			int index_size = sizeof(u16);
			write(index_size);
			write(import_mesh.indices.size());
			for (int i : import_mesh.indices)
			{
				ASSERT(i <= (1 << 16));
				u16 index = (u16)i;
				write(index);
			}
		}
		else
		{
			int index_size = sizeof(import_mesh.indices[0]);
			write(index_size);
			write(import_mesh.indices.size());
			write(&import_mesh.indices[0], sizeof(import_mesh.indices[0]) * import_mesh.indices.size());
		}
		aabb.merge(import_mesh.aabb);
		radius_squared = maximum(radius_squared, import_mesh.radius_squared);
	}

	if (cfg.create_impostor) {
		const int index_size = sizeof(u16);
		write(index_size);
		const u16 indices[] = {0, 1, 2, 0, 2, 3};
		const u32 len = lengthOf(indices);
		write(len);
		write(indices, sizeof(indices));
	}

	for (const ImportMesh& import_mesh : meshes)
	{
		if (!import_mesh.import) continue;
		write((i32)import_mesh.vertex_data.getPos());
		write(import_mesh.vertex_data.getData(), import_mesh.vertex_data.getPos());
	}
	if (cfg.create_impostor) {
		writeImpostorVertices(aabb);
	}

	write(sqrtf(radius_squared) * bounding_shape_scale);
	aabb.min *= bounding_shape_scale;
	aabb.max *= bounding_shape_scale;
	write(aabb);
}


void FBXImporter::writeImpostorMesh(const char* dir, const char* model_name)
{
	const i32 attribute_count = 4;
	write(attribute_count);

	write(Mesh::AttributeSemantic::POSITION);
	write(ffr::AttributeType::FLOAT);
	write((u8)3);

	write(Mesh::AttributeSemantic::NORMAL);
	write(ffr::AttributeType::U8);
	write((u8)4);

	write(Mesh::AttributeSemantic::TANGENT);
	write(ffr::AttributeType::U8);
	write((u8)4);

	write(Mesh::AttributeSemantic::TEXCOORD0);
	write(ffr::AttributeType::FLOAT);
	write((u8)2);

	const StaticString<MAX_PATH_LENGTH + 10> material_name(dir, model_name, "_impostor.mat");
	i32 length = stringLength(material_name);
	write(length);
	write(material_name, length);

	const char* mesh_name = "impostor";
	length = stringLength(mesh_name);
	write(length);
	write(mesh_name, length);
}


void FBXImporter::writeMeshes(const char* src, int mesh_idx, const ImportConfig& cfg)
{
	const PathUtils::FileInfo src_info(src);
	i32 mesh_count = 0;
	if (mesh_idx >= 0) {
		mesh_count = 1;
	}
	else {
		for (ImportMesh& mesh : meshes)
			if (mesh.import) ++mesh_count;
		if (cfg.create_impostor) ++mesh_count;
	}
	write(mesh_count);
	
	auto writeMesh = [&](const ImportMesh& import_mesh ) {
			
		const ofbx::Mesh& mesh = *import_mesh.fbx;

		i32 attribute_count = getAttributeCount(import_mesh);
		write(attribute_count);

		write(Mesh::AttributeSemantic::POSITION);
		write(ffr::AttributeType::FLOAT);
		write((u8)3);
		const ofbx::Geometry* geom = mesh.getGeometry();
		if (geom->getNormals()) {
			write(Mesh::AttributeSemantic::NORMAL);
			write(ffr::AttributeType::U8);
			write((u8)4);
		}
		if (geom->getUVs()) {
			write(Mesh::AttributeSemantic::TEXCOORD0);
			write(ffr::AttributeType::FLOAT);
			write((u8)2);
		}
		if (geom->getColors() && import_vertex_colors) {
			write(Mesh::AttributeSemantic::COLOR0);
			write(ffr::AttributeType::U8);
			write((u8)4);
		}
		if (geom->getTangents()) {
			write(Mesh::AttributeSemantic::TANGENT);
			write(ffr::AttributeType::U8);
			write((u8)4);
		}
		if (import_mesh.is_skinned) {
			write(Mesh::AttributeSemantic::INDICES);
			write(ffr::AttributeType::I16);
			write((u8)4);
			write(Mesh::AttributeSemantic::WEIGHTS);
			write(ffr::AttributeType::FLOAT);
			write((u8)4);
		}

		const ofbx::Material* material = import_mesh.fbx_mat;
		char mat[128];
		getMaterialName(material, mat);
		StaticString<MAX_PATH_LENGTH + 128> mat_id(src_info.m_dir, mat, ".mat");
		const i32 len = stringLength(mat_id.data);
		write(len);
		write(mat_id.data, len);

		char name[256];
		getImportMeshName(import_mesh, name);
		i32 name_len = (i32)stringLength(name);
		write(name_len);
		write(name, stringLength(name));
	};

	if(mesh_idx >= 0) {
		writeMesh(meshes[mesh_idx]);
	}
	else {
		for (ImportMesh& import_mesh : meshes) {
			if (import_mesh.import) writeMesh(import_mesh);
		}
	}

	if (mesh_idx < 0 && cfg.create_impostor) {
		writeImpostorMesh(src_info.m_dir, src_info.m_basename);
	}
}


void FBXImporter::writeSkeleton(const ImportConfig& cfg)
{
	if (ignore_skeleton)
	{
		write((int)0);
		return;
	}

	write(bones.size());

	for (const ofbx::Object*& node : bones)
	{
		const char* name = node->name;
		int len = (int)stringLength(name);
		write(len);
		writeString(name);

		ofbx::Object* parent = node->getParent();
		if (!parent)
		{
			write((int)-1);
		}
		else
		{
			const int idx = bones.indexOf(parent);
			write(idx);
		}

		const ImportMesh* mesh = getAnyMeshFromBone(node, int(&node - bones.begin()));
		Matrix tr = toLumix(getBindPoseMatrix(mesh, node));
		tr.normalizeScale();

		Quat q = fixOrientation(tr.getRotation());
		Vec3 t = fixOrientation(tr.getTranslation());
		write(t * cfg.mesh_scale * fbx_scale);
		write(q);
	}
}


void FBXImporter::writeLODs(const ImportConfig& cfg)
{
	i32 lod_count = 1;
	i32 last_mesh_idx = -1;
	i32 lods[8] = {};
	for (auto& mesh : meshes) {
		if (!mesh.import) continue;

		++last_mesh_idx;
		if (mesh.lod >= lengthOf(cfg.lods_distances)) continue;
		lod_count = mesh.lod + 1;
		lods[mesh.lod] = last_mesh_idx;
	}

	for (u32 i = 1; i < Lumix::lengthOf(lods); ++i) {
		if (lods[i] < lods[i - 1]) lods[i] = lods[i - 1];
	}

	if (cfg.create_impostor) {
		lods[lod_count] = last_mesh_idx + 1;
		++lod_count;
	}

	write((const char*)&lod_count, sizeof(lod_count));

	for (int i = 0; i < lod_count; ++i) {
		i32 to_mesh = lods[i];
		write((const char*)&to_mesh, sizeof(to_mesh));
		float factor = cfg.lods_distances[i] < 0 ? FLT_MAX : cfg.lods_distances[i] * cfg.lods_distances[i];
		write((const char*)&factor, sizeof(factor));
	}
}


int FBXImporter::getAttributeCount(const ImportMesh& mesh) const
{
	int count = 1; // position
	if (mesh.fbx->getGeometry()->getNormals()) ++count;
	if (mesh.fbx->getGeometry()->getUVs()) ++count;
	if (mesh.fbx->getGeometry()->getColors() && import_vertex_colors) ++count;
	if (mesh.fbx->getGeometry()->getTangents()) ++count;
	if (mesh.is_skinned) count += 2;
	return count;
}


bool FBXImporter::areIndices16Bit(const ImportMesh& mesh) const
{
	int vertex_size = getVertexSize(mesh);
	return !(mesh.import && mesh.vertex_data.getPos() / vertex_size > (1 << 16));
}


void FBXImporter::writeModelHeader()
{
	Model::FileHeader header;
	header.magic = 0x5f4c4d4f; // == '_LMO';
	header.version = (u32)Model::FileVersion::LATEST;
	write(header);
}


void FBXImporter::writePhysicsHeader(OS::OutputFile& file) const
{
	PhysicsGeometry::Header header;
	header.m_magic = PhysicsGeometry::HEADER_MAGIC;
	header.m_version = (u32)PhysicsGeometry::Versions::LAST;
	header.m_convex = (u32)make_convex;
	file.write((const char*)&header, sizeof(header));
}


void FBXImporter::writePhysicsTriMesh(OS::OutputFile& file)
{
	i32 count = 0;
	for (auto& mesh : meshes)
	{
		if (mesh.import_physics) count += mesh.indices.size();
	}
	file.write((const char*)&count, sizeof(count));
	int offset = 0;
	for (auto& mesh : meshes)
	{
		if (!mesh.import_physics) continue;
		for (unsigned int j = 0, c = mesh.indices.size(); j < c; ++j)
		{
			u32 index = mesh.indices[j] + offset;
			file.write((const char*)&index, sizeof(index));
		}
		int vertex_size = getVertexSize(mesh);
		int vertex_count = (i32)(mesh.vertex_data.getPos() / vertex_size);
		offset += vertex_count;
	}
}


bool FBXImporter::writePhysics(const char* basename, const char* output_dir)
{
	bool any = false;
	for (const ImportMesh& m : meshes)
	{
		if (m.import_physics)
		{
			any = true;
			break;
		}
	}

	if (!any) return true;

	PathBuilder phy_path(output_dir);
	OS::makePath(phy_path);
	phy_path << "/" << basename << ".phy";
	OS::OutputFile file;
	if (!file.open(phy_path))
	{
		logError("Editor") << "Could not create file " << phy_path;
		return false;
	}

	writePhysicsHeader(file);
	i32 count = 0;
	for (auto& mesh : meshes)
	{
		if (mesh.import_physics) count += (i32)(mesh.vertex_data.getPos() / getVertexSize(mesh));
	}
	file.write((const char*)&count, sizeof(count));
	for (auto& mesh : meshes)
	{
		if (mesh.import_physics)
		{
			int vertex_size = getVertexSize(mesh);
			int vertex_count = (i32)(mesh.vertex_data.getPos() / vertex_size);

			const u8* verts = (const u8*)mesh.vertex_data.getData();

			for (int i = 0; i < vertex_count; ++i)
			{
				Vec3 v = *(Vec3*)(verts + i * vertex_size);
				file.write(&v, sizeof(v));
			}
		}
	}

	if (!make_convex) writePhysicsTriMesh(file);
	file.close();
		
	return true;
}


void FBXImporter::writePrefab(const char* src, const ImportConfig& cfg)
{
	struct SaveEntityGUIDMap : public ISaveEntityGUIDMap
	{
		EntityGUID get(EntityPtr entity) override { return {(u64)entity.index}; }
	};
	OS::OutputFile file;
	PathUtils::FileInfo file_info(src);
	StaticString<MAX_PATH_LENGTH> tmp(file_info.m_dir, "/", file_info.m_basename, ".fab");
	if (!filesystem.open(tmp, Ref(file))) return;

	OutputMemoryStream blob(allocator);
	SaveEntityGUIDMap entity_map;
	TextSerializer serializer(blob, entity_map);

	serializer.write("version", (u32)PrefabVersion::LAST);
	const int count = meshes.size();
	serializer.write("entity_count", count + 1);
	char normalized_tmp_rel[MAX_PATH_LENGTH];
	PathUtils::normalize(tmp, Span(normalized_tmp_rel));
	const u64 prefab = crc32(normalized_tmp_rel);

	serializer.write("prefab", prefab);
	serializer.write("parent", INVALID_ENTITY);
	serializer.write("cmp_end", 0);

	for(int i  = 0; i < meshes.size(); ++i) {
		serializer.write("prefab", prefab | (((u64)i + 1) << 32));
		const EntityRef root = {0};		
		serializer.write("parent", root);
		static const ComponentType MODEL_INSTANCE_TYPE = Reflection::getComponentType("model_instance");

		RigidTransform tr;
		//tr.rot = meshes[i].transform_matrix.getRotation();
		//tr.pos = DVec3(meshes[i].transform_matrix.getTranslation());
		tr.pos = DVec3(0);
		tr.rot = Quat::IDENTITY;
		const float scale = 1;
		serializer.write("transform", tr);
		serializer.write("scale", scale);

		const char* cmp_name = Reflection::getComponentTypeID(MODEL_INSTANCE_TYPE.index);
		const u32 type_hash = Reflection::getComponentTypeHash(MODEL_INSTANCE_TYPE);
		serializer.write(cmp_name, type_hash);
		serializer.write("scene_version", (int)0);
		
		char mesh_name[256];
		getImportMeshName(meshes[i], mesh_name);
		StaticString<MAX_PATH_LENGTH> mesh_path(mesh_name, ".fbx:", src);
		serializer.write("source", (const char*)mesh_path);
		serializer.write("flags", u8(2 /*enabled*/));
		serializer.write("cmp_end", 0);
	}

	file.write(blob.getData(), blob.getPos());
	file.close();
}


void FBXImporter::writeSubmodels(const char* src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION();
	postprocessMeshes(cfg);

	for (int i = 0; i < meshes.size(); ++i) {
		char name[256];
		getImportMeshName(meshes[i], name);

		out_file.clear();
		writeModelHeader();
		writeMeshes(src, i, cfg);
		writeGeometry(i);
		const ofbx::Skin* skin = meshes[i].fbx->getGeometry()->getSkin();
		if (!skin) {
			write((int)0);
		}
		else {
			writeSkeleton(cfg);
		}

		// lods
		const i32 lod_count = 1;
		const i32 to_mesh = 0;
		const float factor = FLT_MAX;
		write(lod_count);
		write(to_mesh);
		write(factor);

		StaticString<MAX_PATH_LENGTH> resource_locator(name, ".fbx:", src);

		compiler.writeCompiledResource(resource_locator, Span((u8*)out_file.getData(), (i32)out_file.getPos()));
	}
}


void FBXImporter::writeModel(const char* src, const ImportConfig& cfg)
{
	PROFILE_FUNCTION();
	postprocessMeshes(cfg);

	auto cmpMeshes = [](const void* a, const void* b) -> int {
		auto a_mesh = static_cast<const ImportMesh*>(a);
		auto b_mesh = static_cast<const ImportMesh*>(b);
		return a_mesh->lod - b_mesh->lod;
	};

	bool import_any_mesh = false;
	for (const ImportMesh& m : meshes) {
		if (m.import) import_any_mesh = true;
	}
	if (!import_any_mesh) return;

	qsort(&meshes[0], meshes.size(), sizeof(meshes[0]), cmpMeshes);
	out_file.clear();
	writeModelHeader();
	writeMeshes(src, -1, cfg);
	writeGeometry(cfg);
	writeSkeleton(cfg);
	writeLODs(cfg);

	compiler.writeCompiledResource(src, Span((u8*)out_file.getData(), (i32)out_file.getPos()));
}


} // namespace Lumix