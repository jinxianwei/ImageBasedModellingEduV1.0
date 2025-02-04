//
// Created by caoqi on 2018/10/01.
//

#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <cassert>

#include "core/depthmap.h"
#include "core/mesh_info.h"
#include "core/mesh_io.h"
#include "core/mesh_io_ply.h"
#include "core/mesh_tools.h"
#include "core/scene.h"
#include "core/view.h"
#include "core/depthmap.h"


struct AppSettings
{
    std::string scenedir;
    std::string outmesh;
    std::string dmname = "depth-L0";
    std::string imagename = "undistorted";
    std::string mask;
    std::string aabb;
    bool with_normals = true;
    bool with_scale = true;
    bool with_conf = true;
    bool poisson_normals = false;
    float min_valid_fraction = 0.0f;
    float scale_factor = 2.5f; /* "Radius" of MVS patch (usually 5x5). */
    std::vector<int> ids;
    int view_id = -1;
    float dd_factor = 5.0f;
    int scale = 0;
};


/**
 * \description 给定像素坐标，深度值以及内参矩阵的逆，求对应三维点的坐标
 * @param x -- 像素坐标x
 * @param y -- 像素坐标y
 * @param depth --深度值即，三维点到相机中心的距离
 * @param invproj -- 内参矩阵的逆
 * @return 三维点坐标
 */
math::Vec3f
pixel_3dpos (std::size_t x, std::size_t y, float depth,
             math::Matrix3f const& invproj) {

    // 图像上的每个像素对应三维中的一条射线(向量）
    math::Vec3f ray = invproj * math::Vec3f
            ((float)x + 0.5f, (float)y + 0.5f, 1.0f);

    // 将射线所在的向量进行归一化，并乘以深度值，得到三维点的坐标
    return ray.normalized() * depth;
}

/**
 * \description 判断深度值是否具有一致性
 * @param widths -- 4x1 4个图像像素对应在三维空间中的宽度，可以理解成空间中长度为widths[i1]的物体
 *                  投影到图像中i1位置的大小刚好为一个像素
 * @param depths -- 深度值 4个图像像素的深度值
 * @param dd_factor -- 判断深度一致性的阈值
 *                 4个像素构成一个grid, 序号分别为
 *                 [0, 1]
 *                 [2, 3]
 * @param i1 -- i1 \in [0,3]
 * @param i2 -- i2 \in [0,3]
 * @return
 */
bool  dm_is_depthdisc (float* widths, float* depths
        , float dd_factor, int i1, int i2) {

    /* Find index that corresponds to smaller depth. */
    int i_min = i1;
    int i_max = i2;
    if (depths[i2] < depths[i1])
        std::swap(i_min, i_max);

    /* Check if indices are a diagonal. */
    if (i1 + i2 == 3)
        dd_factor *= MATH_SQRT2;

    /* Check for depth discontinuity. */
    if (depths[i_max] - depths[i_min] > widths[i_min] * dd_factor)
        return true;

    return false;
}


/**
 * \description 计算1个图像像素在三维空间点p处的长度l，可以理解成空间点p处长度为l的物体
 *              投影到图像大小刚好为一个像素
 * @param x -- 图像像素坐标
 * @param y -- 图像像素坐标
 * @param depth -- 图像深度值
 * @param invproj -- 相机内参矩阵的逆
 * @return
 */
float
pixel_footprint (std::size_t x, std::size_t y, float depth,
                 math::Matrix3f const& invproj) {
    math::Vec3f v = invproj * math::Vec3f
            ((float)x + 0.5f, (float)y + 0.5f, 1.0f);
    return invproj[0] * depth / v.norm();
}

/**
 * \description 创建一个三角面片
 * @param mesh -- 需要创建的三维网格
 * @param vidx -- 图像像素对应三维顶点索引的映射图
 * @param dm   -- 深度图像
 * @param invproj -- 相机内参矩阵的逆矩阵
 * @param i    -- 像素在图像中的全局索引[0, w*h-1]
 * @param tverts -- 三角面片的顶点索引，用的是在局部grid中的索引，即[0, 1]
 *                                                           [2, 3]
 *
 */
void
dm_make_triangle (core::TriangleMesh* mesh, core::Image<unsigned int>& vidx,
                  core::FloatImage const* dm, math::Matrix3f const& invproj,
                  std::size_t i, int* tverts) {

    int const width = vidx.width();
    //int const height = vidx.height();
    core::TriangleMesh::VertexList& verts(mesh->get_vertices());
    core::TriangleMesh::FaceList& faces(mesh->get_faces());

    for (int j = 0; j < 3; ++j) {
        int iidx = i + (tverts[j] % 2) + width * (tverts[j] / 2);
        int x = iidx % width;
        int y = iidx / width;

        // 如果当前像素尚没有对应的三维点
        if (vidx.at(iidx) == MATH_MAX_UINT) {
            /* Add vertex for depth pixel. */
            vidx.at(iidx) = verts.size();
            float depth = dm->at(iidx, 0);
            verts.push_back(pixel_3dpos(x, y, depth, invproj));
        }
        faces.push_back(vidx.at(iidx));
    }
}

/**
 * \description 给定深度图，彩色图像以及相机参数，恢复带有颜色和法向量的三维点云
 * @param dm -- 深度图像
 * @param ci -- 彩色图像
 * @param invproj -- 相机逆投影矩阵
 * @param dd_factor -- 用于判断深度一致性的阈值
 * @return
 */
core::TriangleMesh::Ptr
my_depthmap_triangulate (core::FloatImage::ConstPtr dm, core::ByteImage::ConstPtr ci,
                      math::Matrix3f const& invproj, float dd_factor)
{
    // 深度图像不为空
    if (dm == nullptr)
        throw std::invalid_argument("Null depthmap given");

    // 图像的宽和高
    int const width = dm->width();
    int const height = dm->height();

    // 创建三角网格接结构体
    /* Prepare triangle mesh. */
    core::TriangleMesh::Ptr mesh(core::TriangleMesh::create());

    /* Generate image that maps image pixels to vertex IDs. */
    // 创建映射图，将图像像素映射到三维点的索引
    core::Image<unsigned int> vidx(width, height, 1);
    vidx.fill(MATH_MAX_UINT);

    // 在深度图中遍历2x2 blocks，并且创建三角面片
    /* Iterate over 2x2-blocks in the depthmap and create triangles. */
    int i = 0;
    for (int y = 0; y < height - 1; ++y, ++i) {
        for (int x = 0; x < width - 1; ++x, ++i) {
            /* Cache the four depth values. */
            /*
             * 0, 1
             * 2, 3
             */
            float depths[4] = { dm->at(i, 0),         dm->at(i + 1, 0),
                                dm->at(i + width, 0), dm->at(i + width + 1, 0)
            };

            /* Create a mask representation of the available depth values. */
            /* 创建mask记录深度有效的像素个数
             * mask=0000, 0001, 0010, 0011, 0100, 0101, 0110, 0111, 1000, 1001,
             *      1010, 1011, 1100, 1101, 1110, 1111
             */
            int mask = 0;
            int pixels = 0;
            for (int j = 0; j < 4; ++j){
                if (depths[j] > 0.0f) {
                    mask |= 1 << j;
                    pixels += 1;
                }
            }

            // 至少保证3个深度值是可靠的
            /* At least three valid depth values are required. */
            if (pixels < 3)
                continue;


            /* Possible triangles, vertex indices relative to 2x2 block. */
            /* 可能出现的三角面片对,4个点有2个面片
             */
            int tris[4][3] = {
                    { 0, 2, 1 }, { 0, 3, 1 }, { 0, 2, 3 }, { 1, 2, 3 }
            };

            /* Decide which triangles to issue. */
            /* 决定用哪对面片
             */
            int tri[2] = { 0, 0 };

            switch (mask) {

                case 7: tri[0] = 1; break;  // 0111- 0，1，2
                case 11: tri[0] = 2; break; // 1011- 0，1，3
                case 13: tri[0] = 3; break; // 1101- 0，2，3
                case 14: tri[0] = 4; break; // 1110- 1，2，3
                case 15:                    // 1111- 0，1，2，3
                {
                    // 空圆特性
                    /* Choose the triangulation with smaller diagonal. */
                    float ddiff1 = std::abs(depths[0] - depths[3]);
                    float ddiff2 = std::abs(depths[1] - depths[2]);
                    if (ddiff1 < ddiff2)
                    { tri[0] = 2; tri[1] = 3; }
                    else
                    { tri[0] = 1; tri[1] = 4; }
                    break;
                }
                default: continue;
            }

            /* Omit depth discontinuity detection if dd_factor is zero. */
            if (dd_factor > 0.0f) {
                /* Cache pixel footprints. */
                float widths[4];
                for (int j = 0; j < 4; ++j) {
                    if (depths[j] == 0.0f)
                        continue;
                    widths[j] = pixel_footprint(x + (j % 2), y + (j / 2), depths[j], invproj);// w, h, focal_len);
                }

                // 检查深度不一致性，相邻像素的深度差值不要超过像素宽度(三维空间中）的dd_factor倍
                /* Check for depth discontinuities. */
                for (int j = 0; j < 2 && tri[j] != 0; ++j) {
                    int* tv = tris[tri[j] - 1];
                    #define DM_DD_ARGS widths, depths, dd_factor
                    if (dm_is_depthdisc(DM_DD_ARGS, tv[0], tv[1])) tri[j] = 0;
                    if (dm_is_depthdisc(DM_DD_ARGS, tv[1], tv[2])) tri[j] = 0;
                    if (dm_is_depthdisc(DM_DD_ARGS, tv[2], tv[0])) tri[j] = 0;
                }
            }

            /* Build triangles. */
            for (int j = 0; j < 2; ++j) {
                if (tri[j] == 0) continue;
                #define DM_MAKE_TRI_ARGS mesh.get(), vidx, dm.get(), invproj, i
                dm_make_triangle(DM_MAKE_TRI_ARGS, tris[tri[j] - 1]);
            }
        }
    }

    // 获取颜色
    // 彩色图像不为空，且和深度图像宽和高一致
    if (ci != nullptr && (ci->width() != width || ci->height() != height)){
        std::cout<<"Color image dimension mismatch"<<std::endl;
        return mesh;
    }


    /*计算顶点的颜色*/
    /* Use vertex index mapping to color the mesh. */
    core::TriangleMesh::ColorList& colors(mesh->get_vertex_colors());
    core::TriangleMesh::VertexList const& verts(mesh->get_vertices());
    colors.resize(verts.size());

    // 像素个数
    int num_pixel = vidx.get_pixel_amount();
    for (int i = 0; i < num_pixel; ++i) {
        // 像素没有对应的顶点
        if (vidx[i] == MATH_MAX_UINT)
            continue;

        math::Vec4f color(ci->at(i, 0), 0.0f, 0.0f, 255.0f);
        if (ci->channels() >= 3) {
            color[1] = ci->at(i, 1);
            color[2] = ci->at(i, 2);
        }
        else {
            color[1] = color[2] = color[0];
        }
        colors[vidx[i]] = color / 255.0f;
    }

    return mesh;
}


int main(int argc, char *argv[]){

    if(argc<5){
        std::cout<<"usage: scendir outmeshdir(.ply) scale view_id"<<std::endl;
        return -1;
    }

     AppSettings conf;

    // 场景文件夹
    conf.scenedir = argv[1];
    // 输出网格文件
    conf.outmesh = argv[2];
    // 获取图像尺度
    std::stringstream stream1(argv[3]);
    stream1>>conf.scale;
    // 获取要重建的视角
    std::stringstream stream2(argv[4]);
    stream2>>conf.view_id;

    conf.dmname = std::string("depth-L") + argv[3];
    conf.imagename = (conf.scale == 0)
                ? "undistorted"
                : std::string("undist-L") + argv[3];

    std::cout << "Using depthmap \"" << conf.dmname
              << "\" and color image \"" << conf.imagename << "\"" << std::endl;

    /* Load scene. */
    core::Scene::Ptr scene = core::Scene::create(conf.scenedir);

    /* Iterate over views and get points. */
    core::Scene::ViewList& views(scene->get_views());

    core::View::Ptr view = views[conf.view_id];
    assert(view!= nullptr);

    // 获取存在相机参数
    core::CameraInfo const& cam = view->get_camera();
    assert(cam.flen);

    // 加载深度图
    core::FloatImage::Ptr dm = view->get_float_image(conf.dmname);
    assert(dm != nullptr);

    // 加载彩色图像
    core::ByteImage::Ptr ci;
    if (!conf.imagename.empty())
            ci = view->get_byte_image(conf.imagename);

    std::cout << "Processing view \"" << view->get_name()
              << "\"" << (ci != nullptr ? " (with colors)" : "")
              << "..." << std::endl;

    /********************Triangulate depth map***********************/

    core::TriangleMesh::Ptr mesh;
    // 计算投影矩阵的逆矩阵
    math::Matrix3f invproj;
    cam.fill_inverse_calibration(*invproj, dm->width(), dm->height());

    // 对深度图进行三角化，注意此时的mesh顶点坐标位于相机坐标系中
    mesh = my_depthmap_triangulate(dm, ci, invproj, conf.dd_factor);

    /* Transform mesh to world coordinates. */
    // 将网格从相机坐标系转化到世界坐标系中
    math::Matrix4f ctw;
    cam.fill_cam_to_world(*ctw);
    core::geom::mesh_transform(mesh, ctw);

    if (conf.with_normals)
       mesh->ensure_normals();

    dm.reset();
    ci.reset();
    view->cache_cleanup();
    /***************************************************************/

    /* Write mesh to disc. */
    std::cout << "Writing final point set (" << mesh->get_vertices().size() << " points)..." << std::endl;
    assert(util::string::right(conf.outmesh, 4) == ".ply");
    {
        core::geom::SavePLYOptions opts;
        opts.write_vertex_normals = conf.with_normals;
        core::geom::save_ply_mesh(mesh, conf.outmesh, opts);
    }

    return EXIT_SUCCESS;
}
