/*
 *  Copyright 2019-2024 Diligent Graphics LLC
 *  Copyright 2015-2019 Egor Yusov
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  In no event and under no legal theory, whether in tort (including negligence),
 *  contract, or otherwise, unless required by applicable law (such as deliberate
 *  and grossly negligent acts) or agreed to in writing, shall any Contributor be
 *  liable for any damages, including any direct, indirect, special, incidental,
 *  or consequential damages of any character arising as a result of this License or
 *  out of the use or inability to use the software (including but not limited to damages
 *  for loss of goodwill, work stoppage, computer failure or malfunction, or any and
 *  all other commercial damages or losses), even if such Contributor has been advised
 *  of the possibility of such damages.
 */

#include "pch.h"

#include "ShaderGLImpl.hpp"

#include <array>

#include "RenderDeviceGLImpl.hpp"
#include "DeviceContextGLImpl.hpp"
#include "DataBlobImpl.hpp"
#include "GLSLUtils.hpp"
#include "ShaderToolsCommon.hpp"
#include "GLTypeConversions.hpp"

using namespace Diligent;

namespace Diligent
{

constexpr INTERFACE_ID ShaderGLImpl::IID_InternalImpl;

class ShaderGLImpl::ShaderBuilder
{
public:
    ShaderBuilder(ShaderGLImpl&           Shader,
                  const ShaderCreateInfo& ShaderCI,
                  const CreateInfo&       GLShaderCI) :
        m_Shader{Shader},
        m_LoadConstantBufferReflection{ShaderCI.LoadConstantBufferReflection},
        m_CreateAsynchronously{(ShaderCI.CompileFlags & SHADER_COMPILE_FLAG_ASYNCHRONOUS) != 0 && Shader.GetDevice()->GetDeviceInfo().Features.AsyncShaderCompilation},
        m_ppCompilerOutput{GLShaderCI.ppCompilerOutput},
        m_Program{false}
    {}

    bool Tick()
    {
        VERIFY(m_State != State::Complete && m_State != State::Failed, "The shader is already in final state, this method should not be called");

        if (m_State == State::Default)
        {
            HandleDefaultState();
        }

        if (m_State == State::Compiling)
        {
            HandleCompilingState();
        }

        if (m_State == State::Linking)
        {
            HandleLinkingState();
        }

        if (m_State == State::Failed)
        {
            m_Shader.m_GLShaderObj.Release();
        }

        return m_State == State::Complete || m_State == State::Failed;
    }

private:
    void HandleDefaultState()
    {
        VERIFY_EXPR(m_State == State::Default);

        m_Shader.CompileShader();
        m_State = State::Compiling;
    }

    void HandleCompilingState()
    {
        VERIFY_EXPR(m_State == State::Compiling);

        GLint CompilationComplete = GL_FALSE;
        if (m_CreateAsynchronously)
        {
            glGetShaderiv(m_Shader.m_GLShaderObj, GL_COMPLETION_STATUS_KHR, &CompilationComplete);
        }
        else
        {
            CompilationComplete = GL_TRUE;
        }

        if (CompilationComplete)
        {
            m_State = m_Shader.GetCompileStatus(m_ppCompilerOutput, /*ThrowOnError = */ !m_CreateAsynchronously) ? State::Linking : State::Failed;
        }
    }

    void HandleLinkingState()
    {
        VERIFY_EXPR(m_State == State::Linking);

        RenderDeviceGLImpl*     pDevice    = m_Shader.GetDevice();
        const RenderDeviceInfo& DeviceInfo = pDevice->GetDeviceInfo();

        // Note: we have to always read reflection information in OpenGL as bindings are always assigned at run time.
        if (DeviceInfo.Features.SeparablePrograms /*&& (ShaderCI.CompileFlags & SHADER_COMPILE_FLAG_SKIP_REFLECTION) == 0*/)
        {
            ShaderGLImpl* const ThisShader[] = {&m_Shader};
            if (!m_Program)
            {
                m_Program = LinkProgram(ThisShader, 1, true);
            }

            GLint LinkingComplete = GL_FALSE;
            if (m_CreateAsynchronously)
            {
                glGetProgramiv(m_Program, GL_COMPLETION_STATUS_KHR, &LinkingComplete);
            }
            else
            {
                LinkingComplete = GL_TRUE;
            }

            if (LinkingComplete)
            {
                if (GetProgamLinkStatus(m_Program, ThisShader, 1, /*ThrowOnError = */ !m_CreateAsynchronously))
                {
                    auto pImmediateCtx = pDevice->GetImmediateContext(0);
                    VERIFY_EXPR(pImmediateCtx);
                    auto& GLState = pImmediateCtx->GetContextState();

                    auto pResources = std::make_unique<ShaderResourcesGL>();

                    pResources->LoadUniforms({m_Shader.m_Desc.ShaderType,
                                              m_Shader.m_SourceLanguage == SHADER_SOURCE_LANGUAGE_HLSL ?
                                                  PIPELINE_RESOURCE_FLAG_NONE :            // Reflect samplers as separate for consistency with other backends
                                                  PIPELINE_RESOURCE_FLAG_COMBINED_SAMPLER, // Reflect samplers as combined
                                              m_Program,
                                              GLState,
                                              m_LoadConstantBufferReflection,
                                              m_Shader.m_SourceLanguage});
                    m_Shader.m_pShaderResources.reset(pResources.release());

                    m_State = State::Complete;
                }
                else
                {
                    m_State = State::Failed;
                }
            }
        }
        else
        {
            m_State = State::Complete;
        }
    }

private:
    ShaderGLImpl& m_Shader;

    const bool        m_LoadConstantBufferReflection;
    const bool        m_CreateAsynchronously;
    IDataBlob** const m_ppCompilerOutput;

    // Temporary program object used to load shader resources
    GLObjectWrappers::GLProgramObj m_Program;

    enum class State
    {
        Default,
        Compiling,
        Linking,
        Complete,
        Failed
    };
    State m_State = State::Default;
};

ShaderGLImpl::ShaderGLImpl(IReferenceCounters*     pRefCounters,
                           RenderDeviceGLImpl*     pDeviceGL,
                           const ShaderCreateInfo& ShaderCI,
                           const CreateInfo&       GLShaderCI,
                           bool                    bIsDeviceInternal) noexcept(false) :
    // clang-format off
    TShaderBase
    {
        pRefCounters,
        pDeviceGL,
        ShaderCI.Desc,
        GLShaderCI.DeviceInfo,
        GLShaderCI.AdapterInfo,
        bIsDeviceInternal
    },
    m_SourceLanguage{ShaderCI.SourceLanguage},
    m_GLShaderObj{pDeviceGL != nullptr, GLObjectWrappers::GLShaderObjCreateReleaseHelper{GetGLShaderType(m_Desc.ShaderType)}},
    m_Builder{pDeviceGL != nullptr ? std::make_unique<ShaderBuilder>(*this, ShaderCI, GLShaderCI) : nullptr}
// clang-format on
{
    DEV_CHECK_ERR(ShaderCI.ByteCode == nullptr, "'ByteCode' must be null when shader is created from the source code or a file");
    DEV_CHECK_ERR(ShaderCI.ShaderCompiler == SHADER_COMPILER_DEFAULT, "only default compiler is supported in OpenGL");

    const auto& DeviceInfo  = GLShaderCI.DeviceInfo;
    const auto& AdapterInfo = GLShaderCI.AdapterInfo;

    // Build the full source code string that will contain GLSL version declaration,
    // platform definitions, user-provided shader macros, etc.
    m_GLSLSourceString = BuildGLSLSourceString(
        {
            ShaderCI,
            AdapterInfo,
            DeviceInfo.Features,
            DeviceInfo.Type,
            DeviceInfo.MaxShaderVersion,
            TargetGLSLCompiler::driver,
            DeviceInfo.NDC.MinZ == 0,
        });

    const SHADER_SOURCE_LANGUAGE SourceLang = ParseShaderSourceLanguageDefinition(m_GLSLSourceString);
    if (SourceLang != SHADER_SOURCE_LANGUAGE_DEFAULT)
    {
        // Source language is already defined in the shader source (for instance,
        // it may have been added by the archiver).
        m_SourceLanguage = SourceLang;
    }
    else
    {
        // Add source language definition to the shader source. It may be used by e.g.
        // render state cache when packing shader source into the archive.
        AppendShaderSourceLanguageDefinition(m_GLSLSourceString, ShaderCI.SourceLanguage);
    }

    if (pDeviceGL == nullptr)
        return;

    // Force builder tick
    GetStatus();
}

ShaderGLImpl::~ShaderGLImpl()
{
}

IMPLEMENT_QUERY_INTERFACE2(ShaderGLImpl, IID_ShaderGL, IID_InternalImpl, TShaderBase)

void ShaderGLImpl::CompileShader() noexcept
{
    // Note: there is a simpler way to create the program:
    //m_uiShaderSeparateProg = glCreateShaderProgramv(GL_VERTEX_SHADER, _countof(ShaderStrings), ShaderStrings);
    // NOTE: glCreateShaderProgramv() is considered equivalent to both a shader compilation and a program linking
    // operation. Since it performs both at the same time, compiler or linker errors can be encountered. However,
    // since this function only returns a program object, compiler-type errors will be reported as linker errors
    // through the following API:
    // GLint isLinked = 0;
    // glGetProgramiv(program, GL_LINK_STATUS, &isLinked);
    // The log can then be queried in the same way


    // Each element in the length array may contain the length of the corresponding string
    // (the null character is not counted as part of the string length).
    // Not specifying lengths causes shader compilation errors on Android
    std::array<const char*, 1> ShaderStrings = {};
    std::array<GLint, 1>       Lengths       = {};

    ShaderStrings[0] = m_GLSLSourceString.c_str();
    Lengths[0]       = static_cast<GLint>(m_GLSLSourceString.length());

    // Provide source strings (the strings will be saved in internal OpenGL memory)
    glShaderSource(m_GLShaderObj, static_cast<GLsizei>(ShaderStrings.size()), ShaderStrings.data(), Lengths.data());
    // When the shader is compiled, it will be compiled as if all of the given strings were concatenated end-to-end.
    glCompileShader(m_GLShaderObj);
}

bool ShaderGLImpl::GetCompileStatus(IDataBlob** ppCompilerOutput, bool ThrowOnError) noexcept(false)
{
    GLint compiled = GL_FALSE;
    // Get compilation status
    glGetShaderiv(m_GLShaderObj, GL_COMPILE_STATUS, &compiled);

    std::vector<GLchar> InfoLog;
    {
        int InfoLogLen = 0;
        // The function glGetShaderiv() tells how many bytes to allocate; the length includes the NULL terminator.
        glGetShaderiv(m_GLShaderObj, GL_INFO_LOG_LENGTH, &InfoLogLen);

        if (InfoLogLen > 0)
        {
            InfoLog.resize(InfoLogLen);

            int CharsWritten = 0;
            // Get the log. InfoLogLen is the size of InfoLog. This tells OpenGL how many bytes at maximum it will write
            // charsWritten is a return value, specifying how many bytes it actually wrote. One may pass NULL if he
            // doesn't care
            glGetShaderInfoLog(m_GLShaderObj, InfoLogLen, &CharsWritten, InfoLog.data());
            VERIFY(CharsWritten == InfoLogLen - 1, "Unexpected info log length");

            if (ppCompilerOutput != nullptr)
            {
                // InfoLogLen accounts for null terminator
                auto pOutputDataBlob = DataBlobImpl::Create(InfoLogLen + m_GLSLSourceString.length() + 1);

                char* DataPtr = static_cast<char*>(pOutputDataBlob->GetDataPtr());
                if (!InfoLog.empty())
                {
                    // Copy info log including null terminator
                    memcpy(DataPtr, InfoLog.data(), InfoLogLen);
                }
                // Copy source code including null terminator
                memcpy(DataPtr + InfoLogLen, m_GLSLSourceString.data(), m_GLSLSourceString.length() + 1);

                pOutputDataBlob->QueryInterface(IID_DataBlob, reinterpret_cast<IObject**>(ppCompilerOutput));
            }
        }
    }

    if (!compiled || !InfoLog.empty())
    {
        std::stringstream ss;
        ss << (compiled ? "Compiler output for shader '" : "Failed to compile shader '") << m_Desc.Name << "'";

        if (!InfoLog.empty())
        {
            ss << ":" << std::endl;
            ss.write(InfoLog.data(), InfoLog.size() - 1);
        }
        else if (!compiled)
        {
            ss << " (no shader log available).";
        }

        if (compiled)
        {
            LOG_INFO_MESSAGE(ss.str());
        }
        else
        {
            if (ppCompilerOutput == nullptr)
            {
                // Dump full source code to debug output
                LOG_INFO_MESSAGE("Failed shader full source: \n\n>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n",
                                 m_GLSLSourceString,
                                 "\n<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n\n");
            }

            if (ThrowOnError)
            {
                LOG_ERROR_AND_THROW(ss.str());
            }
            else
            {
                LOG_ERROR_MESSAGE(ss.str());
            }
        }
    }

    return compiled != GL_FALSE;
}

SHADER_STATUS ShaderGLImpl::GetStatus()
{
    if (m_Builder)
    {
        if (m_Builder->Tick())
        {
            m_Builder.reset();
        }
    }
    return m_Builder ? SHADER_STATUS_COMPILING : (m_GLShaderObj ? SHADER_STATUS_READY : SHADER_STATUS_FAILED);
}

GLObjectWrappers::GLProgramObj ShaderGLImpl::LinkProgram(ShaderGLImpl* const* ppShaders, Uint32 NumShaders, bool IsSeparableProgram) noexcept
{
    VERIFY(!IsSeparableProgram || NumShaders == 1, "Number of shaders must be 1 when separable program is created");

    GLObjectWrappers::GLProgramObj GLProg{true};

    // GL_PROGRAM_SEPARABLE parameter must be set before linking!
    if (IsSeparableProgram)
        glProgramParameteri(GLProg, GL_PROGRAM_SEPARABLE, GL_TRUE);

    for (Uint32 i = 0; i < NumShaders; ++i)
    {
        auto* pCurrShader = ppShaders[i];
        glAttachShader(GLProg, pCurrShader->m_GLShaderObj);
        DEV_CHECK_GL_ERROR("glAttachShader() failed");
    }

    //With separable program objects, interfaces between shader stages may
    //involve the outputs from one program object and the inputs from a
    //second program object. For such interfaces, it is not possible to
    //detect mismatches at link time, because the programs are linked
    //separately. When each such program is linked, all inputs or outputs
    //interfacing with another program stage are treated as active. The
    //linker will generate an executable that assumes the presence of a
    //compatible program on the other side of the interface. If a mismatch
    //between programs occurs, no GL error will be generated, but some or all
    //of the inputs on the interface will be undefined.
    glLinkProgram(GLProg);
    DEV_CHECK_GL_ERROR("glLinkProgram() failed");

    return GLProg;
}

bool ShaderGLImpl::GetProgamLinkStatus(GLuint GLProg, ShaderGLImpl* const* ppShaders, Uint32 NumShaders, bool ThrowOnError) noexcept(false)
{
    int IsLinked = GL_FALSE;
    glGetProgramiv(GLProg, GL_LINK_STATUS, &IsLinked);
    DEV_CHECK_GL_ERROR("glGetProgramiv() failed");

    if (!IsLinked)
    {
        int LengthWithNull = 0, Length = 0;
        // Notice that glGetProgramiv is used to get the length for a shader program, not glGetShaderiv.
        // The length of the info log includes a null terminator.
        glGetProgramiv(GLProg, GL_INFO_LOG_LENGTH, &LengthWithNull);

        // The maxLength includes the NULL character
        std::vector<char> shaderProgramInfoLog(LengthWithNull);

        // Notice that glGetProgramInfoLog  is used, not glGetShaderInfoLog.
        glGetProgramInfoLog(GLProg, LengthWithNull, &Length, shaderProgramInfoLog.data());
        VERIFY(Length == LengthWithNull - 1, "Incorrect program info log len");
        if (ThrowOnError)
        {
            LOG_ERROR_AND_THROW("Failed to link shader program:\n", shaderProgramInfoLog.data(), '\n');
        }
        else
        {
            LOG_ERROR("Failed to link shader program:\n", shaderProgramInfoLog.data(), '\n');
        }
    }
    else
    {
        for (Uint32 i = 0; i < NumShaders; ++i)
        {
            auto* pCurrShader = ClassPtrCast<const ShaderGLImpl>(ppShaders[i]);
            glDetachShader(GLProg, pCurrShader->m_GLShaderObj);
            DEV_CHECK_GL_ERROR("glDetachShader() failed");
        }
    }

    return IsLinked != GL_FALSE;
}

Uint32 ShaderGLImpl::GetResourceCount() const
{
    DEV_CHECK_ERR(!m_Builder, "Shader resources are not available until the shader is compiled. Use GetStatus() to check the shader status.");

    if (m_pDevice->GetFeatures().SeparablePrograms)
    {
        return m_pShaderResources ? m_pShaderResources->GetVariableCount() : 0;
    }
    else
    {
        LOG_WARNING_MESSAGE("Shader resource queries are not available when separate shader objects are unsupported");
        return 0;
    }
}

void ShaderGLImpl::GetResourceDesc(Uint32 Index, ShaderResourceDesc& ResourceDesc) const
{
    DEV_CHECK_ERR(!m_Builder, "Shader resources are not available until the shader is compiled. Use GetStatus() to check the shader status.");

    if (m_pDevice->GetFeatures().SeparablePrograms)
    {
        DEV_CHECK_ERR(Index < GetResourceCount(), "Index is out of range");
        if (m_pShaderResources)
            ResourceDesc = m_pShaderResources->GetResourceDesc(Index);
    }
    else
    {
        LOG_WARNING_MESSAGE("Shader resource queries are not available when separate shader objects are unsupported");
    }
}

const ShaderCodeBufferDesc* ShaderGLImpl::GetConstantBufferDesc(Uint32 Index) const
{
    DEV_CHECK_ERR(!m_Builder, "Shader resources are not available until the shader is compiled. Use GetStatus() to check the shader status.");

    if (m_pDevice->GetFeatures().SeparablePrograms)
    {
        if (Index >= GetResourceCount())
        {
            UNEXPECTED("Constant buffer index (", Index, ") is out of range");
            return nullptr;
        }

        // Uniform buffers always go first in the list of resources
        return m_pShaderResources->GetUniformBufferDesc(Index);
    }
    else
    {
        LOG_WARNING_MESSAGE("Shader resource queries are not available when separate shader objects are unsupported");
        return nullptr;
    }
}

} // namespace Diligent
