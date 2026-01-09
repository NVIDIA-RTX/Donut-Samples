/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/Scene.h>
#include <donut/engine/BindingCache.h>
#include <donut/engine/View.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>
#include <nvrhi/utils.h>

using namespace donut;
using namespace donut::math;

#include "lighting_cb.h"

static const char* g_WindowTitle = "Donut Example: Bindless Ray Tracing";

class BindlessRayTracing : public app::ApplicationBase
{
private:
	std::shared_ptr<vfs::RootFileSystem> m_RootFS;

    nvrhi::ShaderLibraryHandle m_ShaderLibrary;
    nvrhi::rt::PipelineHandle m_RayPipeline;
    nvrhi::rt::ShaderTableHandle m_ShaderTable;
    nvrhi::ShaderHandle m_ComputeShader;
    nvrhi::ComputePipelineHandle m_ComputePipeline;
    nvrhi::CommandListHandle m_CommandList;
    nvrhi::BindingLayoutHandle m_BindingLayout;
    nvrhi::BindingSetHandle m_BindingSet;
    nvrhi::BindingLayoutHandle m_BindlessLayout;

    nvrhi::rt::AccelStructHandle m_TopLevelAS;

    nvrhi::BufferHandle m_ConstantBuffer;

    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;
    std::shared_ptr<engine::DescriptorTableManager> m_DescriptorTable;
    std::unique_ptr<engine::Scene> m_Scene;
    nvrhi::TextureHandle m_ColorBuffer;
    app::FirstPersonCamera m_Camera;
    engine::PlanarView m_View;
    std::shared_ptr<engine::DirectionalLight> m_SunLight;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

    bool m_EnableAnimations = true;
    float m_WallclockTime = 0.f;

public:
    using ApplicationBase::ApplicationBase;

    bool Init(bool useRayQuery)
    {
        std::filesystem::path sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/sponza-plus.scene.json";
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/rt_bindless" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        
		m_RootFS = std::make_shared<vfs::RootFileSystem>();
		m_RootFS->mount("/shaders/donut", frameworkShaderPath);
		m_RootFS->mount("/shaders/app", appShaderPath);

		m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), m_RootFS, "/shaders");
		m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(GetDevice(), m_ShaderFactory);
        m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());

        nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
        bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
        bindlessLayoutDesc.firstSlot = 0;
        bindlessLayoutDesc.maxCapacity = 1024;
        bindlessLayoutDesc.registerSpaces = {
            nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
            nvrhi::BindingLayoutItem::Texture_SRV(2)
        };
        m_BindlessLayout = GetDevice()->createBindlessLayout(bindlessLayoutDesc);

        nvrhi::BindingLayoutDesc globalBindingLayoutDesc;
        globalBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
        globalBindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::RayTracingAccelStruct(0),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
            nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
            nvrhi::BindingLayoutItem::Sampler(0),
            nvrhi::BindingLayoutItem::Texture_UAV(0)
        };
        m_BindingLayout = GetDevice()->createBindingLayout(globalBindingLayoutDesc);

        m_DescriptorTable = std::make_shared<engine::DescriptorTableManager>(GetDevice(), m_BindlessLayout);

        auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
        m_TextureCache = std::make_shared<engine::TextureCache>(GetDevice(), nativeFS, m_DescriptorTable);
        
        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, sceneFileName);

        m_SunLight = std::make_shared<engine::DirectionalLight>();
        m_Scene->GetSceneGraph()->AttachLeafNode(m_Scene->GetSceneGraph()->GetRootNode(), m_SunLight);

        m_SunLight->SetDirection(double3(0.1f, -1.0f, -0.15f));
        m_SunLight->angularSize = 0.53f;
        m_SunLight->irradiance = 5.f;

        m_Scene->FinishedLoading(GetFrameIndex());
        
        m_Camera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.8f, 0.f));
        m_Camera.SetMoveSpeed(3.f);

        m_ConstantBuffer = GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
            sizeof(LightingConstants), "LightingConstants", engine::c_MaxRenderPassConstantBufferVersions));

        if (useRayQuery)
        {
            if (!CreateComputePipeline(*m_ShaderFactory))
                return false;
        }
        else
        {
            if (!CreateRayTracingPipeline(*m_ShaderFactory))
                return false;
        }

        m_CommandList = GetDevice()->createCommandList();

        m_CommandList->open();

        CreateAccelStructs(m_CommandList);

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);

        GetDevice()->waitForIdle();

        return true;
    }

    bool LoadScene(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName) override 
    {
        std::unique_ptr<engine::Scene> scene = std::make_unique<engine::Scene>(GetDevice(),
            *m_ShaderFactory, fs, m_TextureCache, m_DescriptorTable, nullptr);

        if (scene->Load(sceneFileName))
        {
            m_Scene = std::move(scene);
            return true;
        }

        return false;
    }

    bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        m_Camera.KeyboardUpdate(key, scancode, action, mods);

        if (key == GLFW_KEY_SPACE && action == GLFW_PRESS)
        {
            m_EnableAnimations = !m_EnableAnimations;
            return true;
        }

        return true;
    }

    bool MousePosUpdate(double xpos, double ypos) override
    {
        m_Camera.MousePosUpdate(xpos, ypos);
        return true;
    }

    bool MouseButtonUpdate(int button, int action, int mods) override
    {
        m_Camera.MouseButtonUpdate(button, action, mods);
        return true;
    }

    bool MouseScrollUpdate(double xoffset, double yoffset) override
    {
        m_Camera.MouseScrollUpdate(xoffset, yoffset);
        return true;
    }

    void Animate(float fElapsedTimeSeconds) override
    {
        m_Camera.Animate(fElapsedTimeSeconds);

        if (IsSceneLoaded() && m_EnableAnimations)
        {
            m_WallclockTime += fElapsedTimeSeconds;
            float offset = 0;

            for (const auto& anim : m_Scene->GetSceneGraph()->GetAnimations())
            {
                float duration = anim->GetDuration();
                float integral;
                float animationTime = std::modf((m_WallclockTime + offset) / duration, &integral) * duration;
                (void)anim->Apply(animationTime);
                offset += 1.0f;
            }
        }

        const char* extraInfo = (m_RayPipeline != nullptr) ? "- using RayPipeline" : "- using RayQuery";
        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle, extraInfo);
    }

    bool CreateRayTracingPipeline(engine::ShaderFactory& shaderFactory)
    {
        std::vector<engine::ShaderMacro> defines = { { "USE_RAY_QUERY", "0" } };
        m_ShaderLibrary = shaderFactory.CreateShaderLibrary("app/rt_bindless.hlsl", &defines);

        if (!m_ShaderLibrary)
            return false;

        nvrhi::rt::PipelineDesc pipelineDesc;
        pipelineDesc.globalBindingLayouts = { m_BindingLayout, m_BindlessLayout };
        pipelineDesc.shaders = {
            { "", m_ShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration), nullptr },
            { "", m_ShaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss), nullptr }
        };

        pipelineDesc.hitGroups = { {
            "HitGroup",
            m_ShaderLibrary->getShader("ClosestHit", nvrhi::ShaderType::ClosestHit),
            m_ShaderLibrary->getShader("AnyHit", nvrhi::ShaderType::AnyHit),
            nullptr, // intersectionShader
            nullptr, // bindingLayout
            false  // isProceduralPrimitive
        } };

        pipelineDesc.maxPayloadSize = sizeof(float) * 6;

        m_RayPipeline = GetDevice()->createRayTracingPipeline(pipelineDesc);

        if (!m_RayPipeline)
            return false;

        auto shaderTableDesc = nvrhi::rt::ShaderTableDesc()
            .enableCaching(3)
            .setDebugName("Shader Table");
        m_ShaderTable = m_RayPipeline->createShaderTable(shaderTableDesc);

        if (!m_ShaderTable)
            return false;

        m_ShaderTable->setRayGenerationShader("RayGen");
        m_ShaderTable->addHitGroup("HitGroup");
        m_ShaderTable->addMissShader("Miss");

        return true;
    }

    bool CreateComputePipeline(engine::ShaderFactory& shaderFactory)
    {
        std::vector<engine::ShaderMacro> defines = { { "USE_RAY_QUERY", "1" } };
        m_ComputeShader = shaderFactory.CreateShader("app/rt_bindless.hlsl", "main", &defines, nvrhi::ShaderType::Compute);

        if (!m_ComputeShader)
            return false;

        auto pipelineDesc = nvrhi::ComputePipelineDesc()
            .setComputeShader(m_ComputeShader)
            .addBindingLayout(m_BindingLayout)
            .addBindingLayout(m_BindlessLayout);

        m_ComputePipeline = GetDevice()->createComputePipeline(pipelineDesc);

        if (!m_ComputePipeline)
            return false;

        return true;
    }

    void GetMeshBlasDesc(engine::MeshInfo& mesh, nvrhi::rt::AccelStructDesc& blasDesc) const
    {
        blasDesc.isTopLevel = false;
        blasDesc.debugName = mesh.name;

        for (const auto& geometry : mesh.geometries)
        {
            nvrhi::rt::GeometryDesc geometryDesc;
            auto & triangles = geometryDesc.geometryData.triangles;
            triangles.indexBuffer = mesh.buffers->indexBuffer;
            triangles.indexOffset = (mesh.indexOffset + geometry->indexOffsetInMesh) * sizeof(uint32_t);
            triangles.indexFormat = nvrhi::Format::R32_UINT;
            triangles.indexCount = geometry->numIndices;
            triangles.vertexBuffer = mesh.buffers->vertexBuffer;
            triangles.vertexOffset = (mesh.vertexOffset + geometry->vertexOffsetInMesh) * sizeof(float3) + mesh.buffers->getVertexBufferRange(engine::VertexAttribute::Position).byteOffset;
            triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
            triangles.vertexStride = sizeof(float3);
            triangles.vertexCount = geometry->numVertices;
            geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;
            geometryDesc.flags = (geometry->material->domain == engine::MaterialDomain::AlphaTested)
                ? nvrhi::rt::GeometryFlags::None
                : nvrhi::rt::GeometryFlags::Opaque;
            blasDesc.bottomLevelGeometries.push_back(geometryDesc);
        }

        // don't compact acceleration structures that are built per frame
        if (mesh.skinPrototype != nullptr)
        {
            blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
        }
        else
        {
            blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace | nvrhi::rt::AccelStructBuildFlags::AllowCompaction;
        }
    }

    void CreateAccelStructs(nvrhi::ICommandList* commandList)
    {
        for (const auto& mesh : m_Scene->GetSceneGraph()->GetMeshes())
        {
            if (mesh->isSkinPrototype)
                continue; // skip the skinning prototypes
            
            nvrhi::rt::AccelStructDesc blasDesc;

            GetMeshBlasDesc(*mesh, blasDesc);

            nvrhi::rt::AccelStructHandle as = GetDevice()->createAccelStruct(blasDesc);

            if (!mesh->skinPrototype)
                nvrhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);

            mesh->accelStruct = as;
        }


        nvrhi::rt::AccelStructDesc tlasDesc;
        tlasDesc.isTopLevel = true;
        tlasDesc.topLevelMaxInstances = m_Scene->GetSceneGraph()->GetMeshInstances().size();
        m_TopLevelAS = GetDevice()->createAccelStruct(tlasDesc);
    }

    void BuildTLAS(nvrhi::ICommandList* commandList, uint32_t frameIndex) const
    {
        commandList->beginMarker("Skinned BLAS Updates");

        // Transition all the buffers to their necessary states before building the BLAS'es to allow BLAS batching
        for (const auto& skinnedInstance : m_Scene->GetSceneGraph()->GetSkinnedMeshInstances())
        {
            if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex)
                continue;

            commandList->setAccelStructState(skinnedInstance->GetMesh()->accelStruct, nvrhi::ResourceStates::AccelStructWrite);
            commandList->setBufferState(skinnedInstance->GetMesh()->buffers->vertexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        }
        commandList->commitBarriers();

        // Now build the BLAS'es
        for (const auto& skinnedInstance : m_Scene->GetSceneGraph()->GetSkinnedMeshInstances())
        {
            if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex)
                continue;
            
            nvrhi::rt::AccelStructDesc blasDesc;
            GetMeshBlasDesc(*skinnedInstance->GetMesh(), blasDesc);
            
            nvrhi::utils::BuildBottomLevelAccelStruct(commandList, skinnedInstance->GetMesh()->accelStruct, blasDesc);
        }
        commandList->endMarker();

        std::vector<nvrhi::rt::InstanceDesc> instances;

        for (const auto& instance : m_Scene->GetSceneGraph()->GetMeshInstances())
        {
            nvrhi::rt::InstanceDesc instanceDesc;
            instanceDesc.bottomLevelAS = instance->GetMesh()->accelStruct;
            assert(instanceDesc.bottomLevelAS);
            instanceDesc.instanceMask = 1;
            instanceDesc.instanceID = instance->GetInstanceIndex();

            auto node = instance->GetNode();
            assert(node);
            dm::affineToColumnMajor(node->GetLocalToWorldTransformFloat(), instanceDesc.transform);

            instances.push_back(instanceDesc);
        }

        // Compact acceleration structures that are tagged for compaction and have finished executing the original build
        commandList->compactBottomLevelAccelStructs();

        commandList->beginMarker("TLAS Update");
        commandList->buildTopLevelAccelStruct(m_TopLevelAS, instances.data(), instances.size());
        commandList->endMarker();
    }


    void BackBufferResizing() override
    { 
        m_ColorBuffer = nullptr;
        m_BindingCache->Clear();
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        const auto& fbinfo = framebuffer->getFramebufferInfo();

        if (!m_ColorBuffer)
        {
            nvrhi::TextureDesc desc;
            desc.width = fbinfo.width;
            desc.height = fbinfo.height;
            desc.isUAV = true;
            desc.keepInitialState = true;
            desc.format = nvrhi::Format::RGBA16_FLOAT;
            desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            desc.debugName = "ColorBuffer";
            m_ColorBuffer = GetDevice()->createTexture(desc);

            nvrhi::BindingSetDesc bindingSetDesc;
            bindingSetDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer),
                nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_TopLevelAS),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_Scene->GetInstanceBuffer()),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_Scene->GetGeometryBuffer()),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_Scene->GetMaterialBuffer()),
                nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_AnisotropicWrapSampler),
                nvrhi::BindingSetItem::Texture_UAV(0, m_ColorBuffer)
            };

            m_BindingSet = GetDevice()->createBindingSet(bindingSetDesc, m_BindingLayout);
        }

        nvrhi::Viewport windowViewport(float(fbinfo.width), float(fbinfo.height));
        m_View.SetViewport(windowViewport);
        m_View.SetMatrices(m_Camera.GetWorldToViewMatrix(), perspProjD3DStyleReverse(dm::PI_f * 0.25f, windowViewport.width() / windowViewport.height(), 0.1f));
        m_View.UpdateCache();

        m_CommandList->open();

        m_Scene->Refresh(m_CommandList, GetFrameIndex());
        BuildTLAS(m_CommandList, GetFrameIndex());
        
        LightingConstants constants = {};
        constants.ambientColor = float4(0.05f);
        m_View.FillPlanarViewConstants(constants.view);
        m_SunLight->FillLightConstants(constants.light);
        m_CommandList->writeBuffer(m_ConstantBuffer, &constants, sizeof(constants));

        if (m_RayPipeline)
        {
            nvrhi::rt::State state;
            state.shaderTable = m_ShaderTable;
            state.bindings = { m_BindingSet, m_DescriptorTable->GetDescriptorTable() };
            m_CommandList->setRayTracingState(state);

            nvrhi::rt::DispatchRaysArguments args;
            args.width = fbinfo.width;
            args.height = fbinfo.height;
            m_CommandList->dispatchRays(args);
        }
        else if (m_ComputePipeline)
        {
            nvrhi::ComputeState state;
            state.pipeline = m_ComputePipeline;
            state.bindings = { m_BindingSet, m_DescriptorTable->GetDescriptorTable() };
            m_CommandList->setComputeState(state);

            m_CommandList->dispatch(
                dm::div_ceil(fbinfo.width, 16),
                dm::div_ceil(fbinfo.height, 16));
        }
        
        m_CommonPasses->BlitTexture(m_CommandList, framebuffer, m_ColorBuffer, m_BindingCache.get());

        m_CommandList->close();
        GetDevice()->executeCommandList(m_CommandList);
    }

};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

    app::DeviceCreationParameters deviceParams;
    deviceParams.enableRayTracingExtensions = true;

    bool useRayQuery = false;
    for (int i = 1; i < __argc; i++)
    {
        if (strcmp(__argv[i], "-rayQuery") == 0)
        {
            useRayQuery = true;
        }
        else if (strcmp(__argv[i], "-debug") == 0)
        {
            deviceParams.enableDebugRuntime = true;
            deviceParams.enableNvrhiValidationLayer = true;
        }
    }

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }

    if (!useRayQuery && !deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline))
    {
        log::fatal("The graphics device does not support Ray Tracing Pipelines");
        return 1;
    }

    if (useRayQuery && !deviceManager->GetDevice()->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        log::fatal("The graphics device does not support Ray Queries");
        return 1;
    }

    {
        BindlessRayTracing example(deviceManager);
        if (example.Init(useRayQuery))
        {
            deviceManager->AddRenderPassToBack(&example);
            deviceManager->RunMessageLoop();
            deviceManager->RemoveRenderPass(&example);
        }
    }
    
    deviceManager->Shutdown();

    delete deviceManager;

    return 0;
}
