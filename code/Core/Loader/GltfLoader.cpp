#include "GltfLoader.h"
#include "Core/FFLog.h"
#include <DirectXMath.h>
#include <filesystem>
#include <cassert>
#include <iostream>
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

// 读取 accessor 的一个元素（float/uint 等）成 float 或 index
static bool ReadVec2(const cgltf_accessor* acc, size_t i, float out[2]){
    return cgltf_accessor_read_float(acc, i, out, 2);
}
static bool ReadVec3(const cgltf_accessor* acc, size_t i, float out[3]){
    return cgltf_accessor_read_float(acc, i, out, 3);
}
static uint32_t ReadIndex(const cgltf_accessor* acc, size_t i){
    cgltf_uint idx = cgltf_accessor_read_index(acc, i);
    return (uint32_t)idx;
}

static std::string DirOf(const std::string& p){
    return std::filesystem::path(p).parent_path().string();
}
static std::string Join(const std::string& a, const std::string& b){
    return (std::filesystem::path(a) / std::filesystem::path(b)).string();
}

static void ApplyFlipLH(SVertexPNT& v){
    // 右手→左手：Z 取反；法线 Z 取反；切线 Z 取反；handedness 需要翻吗？通常保持即可，或按需要修正
    v.pz = -v.pz;
    v.nz = -v.nz;
    v.tz = -v.tz;
    // handedness 不变；如果你发现法线贴图方向不对，可在 shader 里翻 normal.y
}

static void AppendPrimitive(const cgltf_primitive* prim,
                            const std::string& baseDir,
                            bool flipZ, bool flipWinding,
                            SGltfMeshCPU& out)
{
    // --- attributes ---
    const cgltf_accessor* acc_pos = nullptr;
    const cgltf_accessor* acc_nrm = nullptr;
    const cgltf_accessor* acc_uv0 = nullptr;
    const cgltf_accessor* acc_tan = nullptr;
    const cgltf_accessor* acc_col = nullptr;

    // Debug: Print all attributes to check for vertex colors
    for (size_t a=0;a<prim->attributes_count;++a){
        auto& att = prim->attributes[a];
        const char* typeName = "UNKNOWN";
        switch(att.type){
            case cgltf_attribute_type_position: typeName = "POSITION"; break;
            case cgltf_attribute_type_normal: typeName = "NORMAL"; break;
            case cgltf_attribute_type_texcoord: typeName = "TEXCOORD"; break;
            case cgltf_attribute_type_tangent: typeName = "TANGENT"; break;
            case cgltf_attribute_type_color: typeName = "COLOR"; break;
            case cgltf_attribute_type_joints: typeName = "JOINTS"; break;
            case cgltf_attribute_type_weights: typeName = "WEIGHTS"; break;
            default: break;
        }
    }

    for (size_t a=0;a<prim->attributes_count;++a){
        auto& att = prim->attributes[a];
        switch(att.type){
            case cgltf_attribute_type_position: acc_pos = att.data; break;
            case cgltf_attribute_type_normal:   acc_nrm = att.data; break;
            case cgltf_attribute_type_texcoord: if(att.index==0) acc_uv0 = att.data; break;
            case cgltf_attribute_type_tangent:  acc_tan = att.data; break;
            case cgltf_attribute_type_color:    if(att.index==0) acc_col = att.data; break;
            default: break;
        }
    }
    if (!acc_pos) return;

    size_t vcount = acc_pos->count;
    size_t icount = prim->indices ? prim->indices->count : 0;

    // 读取顶点
    std::vector<SVertexPNT> verts(vcount);
    for (size_t i=0;i<vcount;++i){
        float p[3]={0}, n[3]={0}, uv[2]={0}, t[4]={0,0,0,1}, c[4]={1,1,1,1};
        ReadVec3(acc_pos, i, p);
        if (acc_nrm) ReadVec3(acc_nrm, i, n);
        if (acc_uv0) ReadVec2(acc_uv0, i, uv);
        if (acc_tan) cgltf_accessor_read_float(acc_tan, i, t, 4);
        if (acc_col) {
            // glTF COLOR_0 can be VEC3 or VEC4
            if (acc_col->type == cgltf_type_vec4) {
                cgltf_accessor_read_float(acc_col, i, c, 4);
            } else if (acc_col->type == cgltf_type_vec3) {
                cgltf_accessor_read_float(acc_col, i, c, 3);
                c[3] = 1.0f;  // Default alpha
            }
        }

        SVertexPNT v{};
        v.px=p[0]; v.py=p[1]; v.pz=p[2];
        v.nx=n[0]; v.ny=n[1]; v.nz=n[2];
        v.u=uv[0]; v.v=uv[1];
        v.tx=t[0]; v.ty=t[1]; v.tz=t[2]; v.tw=t[3]; // 若 glTF 带切线，直接拿来用
        v.r=c[0]; v.g=c[1]; v.b=c[2]; v.a=c[3];     // Vertex color (default white if not present)
        v.u2=0.0f; v.v2=0.0f;  // UV2 for lightmap (default 0, set by lightmap baker)

        if (flipZ) ApplyFlipLH(v);
        verts[i]=v;
    }

    // 读取索引
    std::vector<uint32_t> indices;
    if (icount>0){
        indices.resize(icount);
        for (size_t i=0;i<icount;++i) indices[i] = ReadIndex(prim->indices, i);
        if (flipWinding){
            for (size_t i=0;i+2<indices.size(); i+=3) std::swap(indices[i+1], indices[i+2]);
        }
    } else {
        // 无索引，转三角列表
        assert(prim->type == cgltf_primitive_type_triangles);
        indices.resize(vcount);
        for (uint32_t i=0;i<vcount;++i) indices[i]=i;
        if (flipWinding){
            for (size_t i=0;i+2<indices.size(); i+=3) std::swap(indices[i+1], indices[i+2]);
        }
    }

    // 贴图路径（只取 baseColor/normal）
    if (prim->material){
        auto* mat = prim->material;
        // baseColor
        if (mat->pbr_metallic_roughness.base_color_texture.texture &&
            mat->pbr_metallic_roughness.base_color_texture.texture->image &&
            mat->pbr_metallic_roughness.base_color_texture.texture->image->uri){
            out.textures.baseColorPath = Join(baseDir, mat->pbr_metallic_roughness.base_color_texture.texture->image->uri);
        }
        // normal
        if (mat->normal_texture.texture &&
            mat->normal_texture.texture->image &&
            mat->normal_texture.texture->image->uri){
            out.textures.normalPath = Join(baseDir, mat->normal_texture.texture->image->uri);
        }
        // metallic-roughness (glTF 2.0 standard: G=Roughness, B=Metallic)
        if (mat->pbr_metallic_roughness.metallic_roughness_texture.texture &&
            mat->pbr_metallic_roughness.metallic_roughness_texture.texture->image &&
            mat->pbr_metallic_roughness.metallic_roughness_texture.texture->image->uri){
            out.textures.metallicRoughnessPath = Join(baseDir, mat->pbr_metallic_roughness.metallic_roughness_texture.texture->image->uri);
        }
    }

    out.mesh.vertices = std::move(verts);
    out.mesh.indices  = std::move(indices);

    // 若 glTF 没有 TANGENT（很常见），用你现有函数生成
    bool has_tangent = (acc_tan != nullptr);
    if (!has_tangent) {
        ComputeTangents(out.mesh.vertices, out.mesh.indices);
    }
}

bool LoadGLTF_PNT(const std::string& gltfPath,
                  std::vector<SGltfMeshCPU>& outMeshes,
                  bool flipZ_to_LH, bool flipWinding)
{
    outMeshes.clear();

    cgltf_options opt{}; cgltf_data* data=nullptr;
    if (cgltf_parse_file(&opt, gltfPath.c_str(), &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&opt, data, gltfPath.c_str()) != cgltf_result_success){ cgltf_free(data); return false; }

    std::string baseDir = DirOf(gltfPath);

    // 遍历节点 → mesh → primitives
    // 这里不把节点矩阵应用到顶点（保持 object-space），你可以在渲染时用 world 矩阵
    for (size_t mi=0; mi<data->meshes_count; ++mi){
        auto* mesh = &data->meshes[mi];
        for (size_t pi=0; pi<mesh->primitives_count; ++pi){
            SGltfMeshCPU m{};
            AppendPrimitive(&mesh->primitives[pi], baseDir, flipZ_to_LH, flipWinding, m);
            if (!m.mesh.vertices.empty()) outMeshes.push_back(std::move(m));
        }
    }

    cgltf_free(data);
    return !outMeshes.empty();
}
