#include "Components/LiveViewport.h"
#include "State/ProjectStore.h"

#include <QLabel>
#include <QVBoxLayout>

#ifdef WORLDFORGE_VULKAN
#include <QFile>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuaternion>
#include <QVulkanFunctions>
#include <QVulkanWindow>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#endif

#ifdef WORLDFORGE_VULKAN
namespace {
namespace wf = worldforge;

QVector3D point(wf::Point3 p) { return {p[0], p[1], p[2]}; }

struct Vertex {
    float position[3];
    float color[3];
};

struct PickTriangle {
    QVector3D a, b, c;
    QString id;
    int face;
};

struct Geometry {
    std::vector<Vertex> triangles, lines;
    std::vector<PickTriangle> picks;
    QVector3D minimum, maximum;
    bool hasBounds = false;

    void append(std::vector<Vertex> &list, QVector3D p, QVector3D color)
    {
        list.push_back({{p.x(), p.y(), p.z()}, {color.x(), color.y(), color.z()}});
    }

    void line(QVector3D a, QVector3D b, QVector3D color)
    {
        append(lines, a, color);
        append(lines, b, color);
    }

    void addMesh(const SceneObject &object, const wf::Mesh &mesh, bool modelOnly, bool selected, int selectedFace)
    {
        QMatrix4x4 transform;
        if (!modelOnly) {
            transform.translate(object.position);
            transform.rotate(QQuaternion::fromEulerAngles(object.rotation));
            transform.scale(object.scale);
        }
        std::vector<QVector3D> positions;
        positions.reserve(mesh.vertices.size());
        for (const auto &vertex : mesh.vertices) {
            const auto p = transform.map(point(vertex));
            positions.push_back(p);
            if (!hasBounds) {
                minimum = maximum = p;
                hasBounds = true;
            }
            for (int axis = 0; axis < 3; ++axis) {
                minimum[axis] = std::min(minimum[axis], p[axis]);
                maximum[axis] = std::max(maximum[axis], p[axis]);
            }
        }
        const auto normals = transform.normalMatrix();
        const QVector3D light = QVector3D(-0.45f, -0.7f, 1.0f).normalized();
        for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
            const auto &face = mesh.faces[i];
            const auto &material = mesh.materials[face.material];
            const bool highlighted = modelOnly && int(i) == selectedFace;
            const QVector3D base = highlighted ? QVector3D(0.65f, 0.27f, 0.95f)
                : QVector3D(material.color[0], material.color[1], material.color[2]);
            const auto defaultNormal = wf::faceNormal(mesh, face);
            const auto addCorner = [&](const wf::MeshCorner &corner) {
                const auto n = corner.normal == wf::Point3{} ? defaultNormal : corner.normal;
                QVector3D worldNormal;
                for (int row = 0; row < 3; ++row)
                    worldNormal[row] = normals(row, 0)*n[0] + normals(row, 1)*n[1] + normals(row, 2)*n[2];
                const float diffuse = std::max(0.0f, QVector3D::dotProduct(worldNormal.normalized(), light));
                const float strength = 0.28f + 0.72f*diffuse;
                append(triangles, positions[corner.vertex], base*strength);
            };
            for (std::size_t corner = 1; corner + 1 < face.corners.size(); ++corner) {
                const auto &a = face.corners[0];
                const auto &b = face.corners[corner];
                const auto &c = face.corners[corner + 1];
                addCorner(a);
                addCorner(b);
                addCorner(c);
                picks.push_back({positions[a.vertex], positions[b.vertex], positions[c.vertex], object.id, int(i)});
            }
            if (selected || modelOnly) {
                const QVector3D color = highlighted ? QVector3D(0.9f, 0.7f, 1.0f)
                    : modelOnly ? QVector3D(0.13f, 0.16f, 0.22f) : QVector3D(0.68f, 0.43f, 0.98f);
                for (std::size_t j = 0; j < face.corners.size(); ++j)
                    line(positions[face.corners[j].vertex],
                         positions[face.corners[(j+1)%face.corners.size()].vertex], color);
            }
        }
    }
};

float hitDistance(const PickTriangle &triangle, QVector3D origin, QVector3D direction)
{
    constexpr float miss = std::numeric_limits<float>::infinity();
    const auto u = triangle.b - triangle.a;
    const auto v = triangle.c - triangle.a;
    const auto p = QVector3D::crossProduct(direction, v);
    const auto determinant = QVector3D::dotProduct(u, p);
    if (std::abs(determinant) < 1e-8f)
        return miss;
    const auto delta = origin - triangle.a;
    const float s = QVector3D::dotProduct(delta, p) / determinant;
    const auto q = QVector3D::crossProduct(delta, u);
    const float t = QVector3D::dotProduct(direction, q) / determinant;
    const float depth = QVector3D::dotProduct(v, q) / determinant;
    return s >= 0 && t >= 0 && s + t <= 1 && depth > 0 ? depth : miss;
}
}

class VulkanSceneRenderer;

class VulkanSceneWindow : public QVulkanWindow {
public:
    VulkanSceneWindow(ProjectStore &store, bool modelOnly, LiveViewport &owner)
        : store_(store), modelOnly_(modelOnly), owner_(owner) {}

    QVulkanWindowRenderer *createRenderer() override;
    void invalidate()
    {
        dirty_ = true;
        requestUpdate();
    }
    void setFace(int face)
    {
        if (face_ != face) {
            face_ = face;
            invalidate();
        }
    }
    void frame()
    {
        auto &data = geometry();
        target_ = data.hasBounds ? (data.minimum + data.maximum)*0.5f : QVector3D();
        distance_ = data.hasBounds ? std::max(0.3f, (data.maximum - data.minimum).length()*1.55f) : 5.0f;
        framed_ = data.hasBounds;
        requestUpdate();
    }
    bool rendered() const { return rendered_; }
    void report(const QString &status) { emit owner_.rendererStatusChanged(status); }

protected:
    void mousePressEvent(QMouseEvent *event) override { previous_ = event->position(); }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        const auto delta = event->position() - previous_;
        previous_ = event->position();
        if (event->buttons().testFlag(Qt::RightButton)) {
            yaw_ -= float(delta.x())*0.008f;
            pitch_ = std::clamp(pitch_ + float(delta.y())*0.008f, -1.48f, 1.48f);
            requestUpdate();
        } else if (event->buttons().testFlag(Qt::MiddleButton)) {
            const auto side = QVector3D(-std::sin(yaw_), std::cos(yaw_), 0);
            const auto up = QVector3D::crossProduct(viewDirection(), side).normalized();
            target_ += (side*float(-delta.x()) + up*float(delta.y())) * distance_ / std::max(height(), 1);
            requestUpdate();
        }
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !rendered_)
            return;
        bool invertible = false;
        const auto inverse = camera(size()).inverted(&invertible);
        if (!invertible)
            return;
        const float x = float(event->position().x()*2.0/width() - 1.0);
        const float y = float(event->position().y()*2.0/height() - 1.0);
        const auto origin = inverse.map(QVector3D(x, y, 0));
        const auto direction = (inverse.map(QVector3D(x, y, 1)) - origin).normalized();
        const PickTriangle *best = nullptr;
        float distance = std::numeric_limits<float>::infinity();
        for (const auto &triangle : geometry().picks) {
            const float hit = hitDistance(triangle, origin, direction);
            if (hit < distance) {
                distance = hit;
                best = &triangle;
            }
        }
        if (modelOnly_)
            emit owner_.facePicked(best ? best->face : -1);
        else
            store_.select(best ? best->id : QString());
    }
    void wheelEvent(QWheelEvent *event) override
    {
        distance_ = std::clamp(distance_*std::exp(-float(event->angleDelta().y())/650.0f), 0.02f, 1000000.0f);
        requestUpdate();
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_F)
            frame();
    }

private:
    friend class VulkanSceneRenderer;
    QVector3D viewDirection() const
    {
        return {std::cos(yaw_)*std::cos(pitch_), std::sin(yaw_)*std::cos(pitch_), std::sin(pitch_)};
    }
    QMatrix4x4 camera(QSize size)
    {
        QMatrix4x4 projection, view;
        const float aspect = float(size.width()) / std::max(size.height(), 1);
        projection.perspective(45.0f, aspect, std::max(0.001f, distance_/1000), std::max(100.0f, distance_*100.0f));
        view.lookAt(target_ + viewDirection()*distance_, target_, {0, 0, 1});
        return clipCorrectionMatrix()*projection*view;
    }
    Geometry &geometry()
    {
        if (!dirty_)
            return geometry_;
        geometry_ = {};
        dirty_ = false;
        if (const auto *project = store_.project()) {
            for (const auto &object : project->objects) {
                const bool selected = object.id == store_.selection();
                if ((modelOnly_ && !selected) || (!modelOnly_ && !object.visible))
                    continue;
                if (const auto *asset = store_.meshAsset(object.meshAsset))
                    geometry_.addMesh(object, *asset->mesh, modelOnly_, selected, face_);
            }
        }
        const float z = geometry_.hasBounds ? geometry_.minimum.z() - 0.003f : 0;
        for (int i = -10; i <= 10; ++i) {
            const auto v = float(i);
            geometry_.line({v,-10,z}, {v,10,z}, i ? QVector3D(0.20f,0.23f,0.29f) : QVector3D(0.30f,0.55f,0.32f));
            geometry_.line({-10,v,z}, {10,v,z}, i ? QVector3D(0.20f,0.23f,0.29f) : QVector3D(0.64f,0.28f,0.32f));
        }
        return geometry_;
    }
    ProjectStore &store_;
    bool modelOnly_;
    LiveViewport &owner_;
    bool dirty_ = true;
    bool framed_ = false;
    bool rendered_ = false;
    int face_ = -1;
    Geometry geometry_;
    QVector3D target_;
    float distance_ = 5;
    float yaw_ = -0.85f;
    float pitch_ = 0.57f;
    QPointF previous_;
};

class VulkanSceneRenderer : public QVulkanWindowRenderer {
public:
    explicit VulkanSceneRenderer(VulkanSceneWindow &window) : window_(window) {}

    void initResources() override
    {
        device_ = window_.device();
        vk_ = window_.vulkanInstance()->deviceFunctions(device_);
        frames_.resize(window_.concurrentFrameCount());
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkPushConstantRange camera{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &camera;
        if (vk_->vkCreatePipelineLayout(device_, &layout, nullptr, &layout_) != VK_SUCCESS)
            window_.report(QObject::tr("Vulkan pipeline layout creation failed."));
    }
    void initSwapChainResources() override
    {
        if (layout_) {
            triangles_ = pipeline(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true);
            lines_ = pipeline(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false);
        }
        if (!triangles_ || !lines_)
            window_.report(QObject::tr("Vulkan shader or pipeline creation failed."));
    }
    void releaseSwapChainResources() override
    {
        if (triangles_)
            vk_->vkDestroyPipeline(device_, triangles_, nullptr);
        if (lines_)
            vk_->vkDestroyPipeline(device_, lines_, nullptr);
        triangles_ = lines_ = VK_NULL_HANDLE;
    }
    void releaseResources() override
    {
        for (auto &frame : frames_)
            destroy(frame);
        frames_.clear();
        if (layout_)
            vk_->vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
        window_.rendered_ = false;
    }
    void startNextFrame() override
    {
        auto &geometry = window_.geometry();
        if (!window_.framed_ && geometry.hasBounds)
            window_.frame();
        const auto size = window_.swapChainImageSize();
        const auto command = window_.currentCommandBuffer();
        const VkExtent2D extent{uint32_t(size.width()), uint32_t(size.height())};
        const VkRect2D area{{0,0}, extent};
        VkClearValue clear[2]{};
        clear[0].color = {{0.045f,0.059f,0.080f,1.0f}};
        clear[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass = window_.defaultRenderPass();
        pass.framebuffer = window_.currentFramebuffer();
        pass.renderArea = area;
        pass.clearValueCount = 2;
        pass.pClearValues = clear;

        auto &frame = frames_.at(window_.currentFrame());
        const bool ready = triangles_ && lines_ && upload(frame, geometry);
        vk_->vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
        if (ready) {
            const VkViewport viewport{0, 0, float(size.width()), float(size.height()), 0, 1};
            vk_->vkCmdSetViewport(command, 0, 1, &viewport);
            vk_->vkCmdSetScissor(command, 0, 1, &area);
            const auto camera = window_.camera(size);
            vk_->vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, camera.constData());
            const VkDeviceSize offset = 0;
            vk_->vkCmdBindVertexBuffers(command, 0, 1, &frame.buffer, &offset);
            if (!geometry.triangles.empty()) {
                vk_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, triangles_);
                vk_->vkCmdDraw(command, uint32_t(geometry.triangles.size()), 1, 0, 0);
            }
            vk_->vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, lines_);
            vk_->vkCmdDraw(command, uint32_t(geometry.lines.size()), 1, uint32_t(geometry.triangles.size()), 0);
        }
        vk_->vkCmdEndRenderPass(command);
        if (ready && !window_.rendered_) {
            window_.rendered_ = true;
            window_.report(QObject::tr("Vulkan · %1 · RMB orbit · MMB pan · Wheel zoom · F frame")
                .arg(QString::fromUtf8(window_.physicalDeviceProperties()->deviceName)));
        } else if (!ready && triangles_ && lines_) {
            window_.report(QObject::tr("Vulkan could not upload the scene geometry."));
        }
        window_.frameReady();
    }
    void logicalDeviceLost() override { window_.report(QObject::tr("Vulkan device lost.")); }
    void physicalDeviceLost() override { window_.report(QObject::tr("Vulkan graphics device is unavailable.")); }

private:
    struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; VkDeviceSize capacity{}; };
    void destroy(Buffer &buffer)
    {
        if (buffer.buffer)
            vk_->vkDestroyBuffer(device_, buffer.buffer, nullptr);
        if (buffer.memory)
            vk_->vkFreeMemory(device_, buffer.memory, nullptr);
        buffer = {};
    }
    bool upload(Buffer &buffer, const Geometry &geometry)
    {
        const auto triangleBytes = geometry.triangles.size()*sizeof(Vertex);
        const auto lineBytes = geometry.lines.size()*sizeof(Vertex);
        const auto bytes = triangleBytes + lineBytes;
        if (buffer.capacity < bytes) {
            destroy(buffer);
            VkBufferCreateInfo desc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            desc.size = std::max<VkDeviceSize>(bytes, 4096);
            desc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            if (vk_->vkCreateBuffer(device_, &desc, nullptr, &buffer.buffer) != VK_SUCCESS)
                return false;
            VkMemoryRequirements requirements;
            vk_->vkGetBufferMemoryRequirements(device_, buffer.buffer, &requirements);
            VkPhysicalDeviceMemoryProperties memory;
            window_.vulkanInstance()->functions()->vkGetPhysicalDeviceMemoryProperties(window_.physicalDevice(), &memory);
            uint32_t index = 0;
            constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            while (index < memory.memoryTypeCount && (!(requirements.memoryTypeBits & (1u<<index))
                   || (memory.memoryTypes[index].propertyFlags & flags) != flags))
                ++index;
            const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, requirements.size, index};
            if (index == memory.memoryTypeCount || vk_->vkAllocateMemory(device_, &allocation, nullptr, &buffer.memory) != VK_SUCCESS
                || vk_->vkBindBufferMemory(device_, buffer.buffer, buffer.memory, 0) != VK_SUCCESS) {
                destroy(buffer);
                return false;
            }
            buffer.capacity = desc.size;
        }
        void *target = nullptr;
        if (vk_->vkMapMemory(device_, buffer.memory, 0, bytes, 0, &target) != VK_SUCCESS)
            return false;
        if (triangleBytes)
            std::memcpy(target, geometry.triangles.data(), triangleBytes);
        std::memcpy(static_cast<char *>(target) + triangleBytes, geometry.lines.data(), lineBytes);
        vk_->vkUnmapMemory(device_, buffer.memory);
        return true;
    }
    VkShaderModule shader(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return VK_NULL_HANDLE;
        const auto code = file.readAll();
        if (code.isEmpty() || code.size()%4)
            return VK_NULL_HANDLE;
        std::vector<uint32_t> words(code.size()/4);
        std::memcpy(words.data(), code.constData(), code.size());
        const VkShaderModuleCreateInfo desc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                            std::size_t(code.size()), words.data()};
        VkShaderModule module{};
        return vk_->vkCreateShaderModule(device_, &desc, nullptr, &module) == VK_SUCCESS ? module : VK_NULL_HANDLE;
    }
    VkPipeline pipeline(VkPrimitiveTopology topology, bool depthWrite)
    {
        const auto vertex = shader(":/shaders/Scene.vert.spv");
        const auto fragment = shader(":/shaders/Scene.frag.spv");
        VkPipeline result{};
        if (vertex && fragment) {
            const VkPipelineShaderStageCreateInfo stages[]{
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr},
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr}};
            const VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
            const VkVertexInputAttributeDescription attributes[]{
                {0,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex, position)}, {1,0,VK_FORMAT_R32G32B32_SFLOAT,offsetof(Vertex, color)}};
            const VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                nullptr, 0, 1, &binding, 2, attributes};
            const VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                nullptr, 0, topology, VK_FALSE};
            const VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                nullptr, 0, 1, nullptr, 1, nullptr};
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            samples.rasterizationSamples = window_.sampleCountFlagBits();
            VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            depth.depthTestEnable = VK_TRUE;
            depth.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
            depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo colors{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            colors.attachmentCount = 1;
            colors.pAttachments = &blend;
            const VkDynamicState states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 2, states};
            VkGraphicsPipelineCreateInfo desc{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            desc.stageCount = 2;
            desc.pStages = stages;
            desc.pVertexInputState = &input;
            desc.pInputAssemblyState = &assembly;
            desc.pViewportState = &view;
            desc.pRasterizationState = &raster;
            desc.pMultisampleState = &samples;
            desc.pDepthStencilState = &depth;
            desc.pColorBlendState = &colors;
            desc.pDynamicState = &dynamic;
            desc.layout = layout_;
            desc.renderPass = window_.defaultRenderPass();
            if (vk_->vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &desc, nullptr, &result) != VK_SUCCESS)
                result = VK_NULL_HANDLE;
        }
        if (vertex)
            vk_->vkDestroyShaderModule(device_, vertex, nullptr);
        if (fragment)
            vk_->vkDestroyShaderModule(device_, fragment, nullptr);
        return result;
    }
    VulkanSceneWindow &window_;
    VkDevice device_{};
    QVulkanDeviceFunctions *vk_{};
    VkPipelineLayout layout_{};
    VkPipeline triangles_{};
    VkPipeline lines_{};
    std::vector<Buffer> frames_;
};

QVulkanWindowRenderer *VulkanSceneWindow::createRenderer() { return new VulkanSceneRenderer(*this); }
#endif

LiveViewport::LiveViewport(ProjectStore &store, bool modelOnly, QWidget *parent)
    : QWidget(parent), status_(new QLabel(this))
{
    setObjectName("liveViewport");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    status_->setContentsMargins(8, 3, 8, 3);
    status_->setWordWrap(true);
    connect(this, &LiveViewport::rendererStatusChanged, status_, &QLabel::setText);
#ifdef WORLDFORGE_VULKAN
    instance_ = std::make_shared<QVulkanInstance>();
    if (instance_->supportedLayers().contains("VK_LAYER_KHRONOS_validation"))
        instance_->setLayers({"VK_LAYER_KHRONOS_validation"});
    if (instance_->create()) {
        window_ = new VulkanSceneWindow(store, modelOnly, *this);
        window_->setVulkanInstance(instance_.get());
        container_ = QWidget::createWindowContainer(window_, this);
        container_->setMinimumSize(160, 140);
        container_->setFocusPolicy(Qt::StrongFocus);
        layout->addWidget(container_, 1);
        status_->setText(tr("Starting Vulkan viewport…"));
        connect(&store, &ProjectStore::sceneChanged, this, [this] { window_->invalidate(); });
        connect(&store, &ProjectStore::selectionChanged, this, [this, modelOnly] {
            window_->invalidate();
            if (modelOnly)
                window_->frame();
        });
    } else {
        status_->setText(tr("Vulkan could not start (VkResult %1). Check the graphics driver.").arg(int(instance_->errorCode())));
        layout->addStretch(1);
    }
#else
    Q_UNUSED(store);
    Q_UNUSED(modelOnly);
    status_->setText(tr("This build has no Vulkan viewport. Install Vulkan headers and glslang, then rebuild."));
    layout->addStretch(1);
#endif
    layout->addWidget(status_);
}

LiveViewport::~LiveViewport()
{
    delete container_;
}

void LiveViewport::frame()
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->frame();
#endif
}

void LiveViewport::setFace(int face)
{
#ifdef WORLDFORGE_VULKAN
    if (window_)
        window_->setFace(face);
#else
    Q_UNUSED(face);
#endif
}

bool LiveViewport::hasRenderedFrame() const
{
#ifdef WORLDFORGE_VULKAN
    return window_ && window_->rendered();
#else
    return false;
#endif
}
