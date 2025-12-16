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

#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/ThreadPool.h>
#include <donut/engine/Scene.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/BindingCache.h>
#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/core/math/math.h>

using namespace donut;

static const char* g_WindowTitle = "Donut Example: Threaded Rendering";

class ThreadedRendering : public app::ApplicationBase
{
private:
    std::shared_ptr<vfs::RootFileSystem> m_RootFS;

    nvrhi::CommandListHandle m_CommandList;
    std::array<nvrhi::CommandListHandle, 6> m_FaceCommandLists;

    bool m_UseThreads = true;
    std::unique_ptr<engine::ThreadPool> m_ThreadPool;
    
    nvrhi::TextureHandle m_DepthBuffer;
    nvrhi::TextureHandle m_ColorBuffer;
    std::unique_ptr<engine::FramebufferFactory> m_Framebuffer;
    
    std::unique_ptr<render::ForwardShadingPass> m_ForwardShadingPass;
    std::shared_ptr<engine::ShaderFactory> m_ShaderFactory;
    std::unique_ptr<engine::Scene> m_Scene;
    std::unique_ptr<engine::BindingCache> m_BindingCache;

    app::FirstPersonCamera m_Camera;
    engine::CubemapView m_CubemapView;

public:
    using ApplicationBase::ApplicationBase;
    
    bool Init()
    {
        std::filesystem::path sceneFileName = app::GetDirectoryWithExecutable().parent_path() / "media/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf";
        std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
        
        m_RootFS = std::make_shared<vfs::RootFileSystem>();
        m_RootFS->mount("/shaders/donut", frameworkShaderPath);

        m_ThreadPool = std::make_unique<engine::ThreadPool>();

        m_ShaderFactory = std::make_shared<engine::ShaderFactory>(GetDevice(), m_RootFS, "/shaders");
        m_CommonPasses = std::make_shared<engine::CommonRenderPasses>(GetDevice(), m_ShaderFactory);
        m_BindingCache = std::make_unique<engine::BindingCache>(GetDevice());

        auto nativeFS = std::make_shared<vfs::NativeFileSystem>();
        m_TextureCache = std::make_shared<engine::TextureCache>(GetDevice(), nativeFS, nullptr);

        SetAsynchronousLoadingEnabled(false);
        BeginLoadingScene(nativeFS, sceneFileName);

        m_Scene->FinishedLoading(GetFrameIndex());
        
        m_Camera.LookAt(dm::float3(0.f, 1.8f, 0.f), dm::float3(1.f, 1.8f, 0.f));
        m_Camera.SetMoveSpeed(3.f);
        
        m_CommandList = GetDevice()->createCommandList();
        for (auto& commandList : m_FaceCommandLists)
        {
            commandList = GetDevice()->createCommandList(nvrhi::CommandListParameters()
                .setEnableImmediateExecution(false));
        }

        m_ForwardShadingPass = std::make_unique<render::ForwardShadingPass>(GetDevice(), m_CommonPasses);
        render::ForwardShadingPass::CreateParameters forwardParams;
        forwardParams.numConstantBufferVersions = 128;
        m_ForwardShadingPass->Init(*m_ShaderFactory, forwardParams);

        CreateRenderTargets();

        return true;
    }
    
    void CreateRenderTargets()
    {
        auto textureDesc = nvrhi::TextureDesc()
            .setDimension(nvrhi::TextureDimension::TextureCube)
            .setArraySize(6)
            .setWidth(1024)
            .setHeight(1024)
            .setClearValue(nvrhi::Color(0.f))
            .setIsRenderTarget(true)
            .setKeepInitialState(true);

        m_ColorBuffer = GetDevice()->createTexture(textureDesc
            .setDebugName("ColorBuffer")
            .setFormat(nvrhi::Format::SRGBA8_UNORM)
            .setInitialState(nvrhi::ResourceStates::RenderTarget));

        m_DepthBuffer = GetDevice()->createTexture(textureDesc
            .setDebugName("DepthBuffer")
            .setFormat(nvrhi::Format::D32)
            .setInitialState(nvrhi::ResourceStates::DepthWrite));

        m_CubemapView.SetArrayViewports(textureDesc.width, 0);

        m_Framebuffer = std::make_unique<engine::FramebufferFactory>(GetDevice());
        m_Framebuffer->RenderTargets.push_back(m_ColorBuffer);
        m_Framebuffer->DepthTarget = m_DepthBuffer;
    }

    bool LoadScene(std::shared_ptr<vfs::IFileSystem> fs, const std::filesystem::path& sceneFileName) override 
    {
        std::unique_ptr<engine::Scene> scene = std::make_unique<engine::Scene>(GetDevice(),
            *m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);

        if (scene->LoadWithThreadPool(sceneFileName, m_ThreadPool.get()))
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
            m_UseThreads = !m_UseThreads;
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

    void Animate(float fElapsedTimeSeconds) override
    {
        m_Camera.Animate(fElapsedTimeSeconds);

        GetDeviceManager()->SetInformativeWindowTitle(g_WindowTitle, true, m_UseThreads ? "(With threads)" : "(No threads)");
    }

    void BackBufferResizing() override
    { 
        m_BindingCache->Clear();
    }

    void RenderCubeFace(int face)
    {
        const engine::IView* faceView = m_CubemapView.GetChildView(engine::ViewType::PLANAR, face);

        nvrhi::ICommandList* commandList = m_FaceCommandLists[face];
        commandList->open();
        commandList->clearDepthStencilTexture(m_DepthBuffer, faceView->GetSubresources(), true, 0.f, false, 0);
        commandList->clearTextureFloat(m_ColorBuffer, faceView->GetSubresources(), nvrhi::Color(0.f));

        render::ForwardShadingPass::Context context;
        m_ForwardShadingPass->PrepareLights(context, commandList, {}, 1.0f, 0.3f, {});

        commandList->setEnableAutomaticBarriers(false);
        commandList->setResourceStatesForFramebuffer(m_Framebuffer->GetFramebuffer(*faceView));
        commandList->commitBarriers();

        render::InstancedOpaqueDrawStrategy strategy;

        render::RenderCompositeView(commandList, faceView, faceView, *m_Framebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(), strategy, *m_ForwardShadingPass, context);

        commandList->setEnableAutomaticBarriers(true);

        commandList->close();
    }

    void Render(nvrhi::IFramebuffer* framebuffer) override
    {
        dm::affine viewMatrix = m_Camera.GetWorldToViewMatrix();
        m_CubemapView.SetTransform(viewMatrix, 0.1f, 100.f);
        m_CubemapView.UpdateCache();

        if (m_UseThreads)
        {
            for (int face = 0; face < 6; face++)
            {
                m_ThreadPool->AddTask([this, face]() { RenderCubeFace(face); });
            }
        }
        else
        {
            for (int face = 0; face < 6; face++)
            {
                RenderCubeFace(face);
            }
        }
        
        m_CommandList->open();

        const std::vector<std::pair<int, int>> faceLayout = {
            { 3, 1 },
            { 1, 1 },
            { 2, 0 },
            { 2, 2 },
            { 2, 1 },
            { 0, 1 }
        };

        auto fbinfo = framebuffer->getFramebufferInfo();
        int faceSize = std::min(fbinfo.width / 4, fbinfo.height / 3);

        for (int face = 0; face < 6; face++)
        {
            nvrhi::Viewport viewport;
            viewport.minX = float(faceLayout[face].first * faceSize);
            viewport.maxX = viewport.minX + float(faceSize);
            viewport.minY = float(faceLayout[face].second * faceSize);
            viewport.maxY = viewport.minY + float(faceSize);
            viewport.minZ = 0.f;
            viewport.maxZ = 1.f;

            engine::BlitParameters blitParams;
            blitParams.targetFramebuffer = framebuffer;
            blitParams.targetViewport = viewport;
            blitParams.sourceTexture = m_ColorBuffer;
            blitParams.sourceArraySlice = face;
            m_CommonPasses->BlitTexture(m_CommandList, blitParams, m_BindingCache.get());
        }
        
        m_CommandList->close();

        if (m_UseThreads)
        {
            m_ThreadPool->WaitForTasks();
        }

        nvrhi::ICommandList* commandLists[] = {
            m_FaceCommandLists[0],
            m_FaceCommandLists[1],
            m_FaceCommandLists[2],
            m_FaceCommandLists[3],
            m_FaceCommandLists[4],
            m_FaceCommandLists[5],
            m_CommandList
        };
        
        GetDevice()->executeCommandLists(commandLists, std::size(commandLists));
    }
};

#ifdef WIN32
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
#else
int main(int __argc, const char** __argv)
#endif
{
    nvrhi::GraphicsAPI api = app::GetGraphicsAPIFromCommandLine(__argc, __argv);
    if (api == nvrhi::GraphicsAPI::D3D11)
    {
        log::error("The Threaded Rendering example does not support D3D11.");
        return 1;
    }

    app::DeviceManager* deviceManager = app::DeviceManager::Create(api);

    app::DeviceCreationParameters deviceParams;
    deviceParams.backBufferWidth = 1024; // window size matches the layout of the rendered cube faces
    deviceParams.backBufferHeight = 768;
#ifdef _DEBUG
    deviceParams.enableDebugRuntime = true; 
    deviceParams.enableNvrhiValidationLayer = true;
#endif

    if (!deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, g_WindowTitle))
    {
        log::fatal("Cannot initialize a graphics device with the requested parameters");
        return 1;
    }
    
    {
        ThreadedRendering example(deviceManager);
        if (example.Init())
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
