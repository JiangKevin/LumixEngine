#include "glcorearb.h"

#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
	#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT
	#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#endif
#ifndef GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
	#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#endif

#ifndef GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX
    #define GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX          0x9047
    #define GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX    0x9048
    #define GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX  0x9049
    #define GPU_MEMORY_INFO_EVICTION_COUNT_NVX            0x904A
    #define GPU_MEMORY_INFO_EVICTED_MEMORY_NVX            0x904B
#endif

typedef const GLubyte* (APIENTRY* PFNGLGETSTRINGIPROC) (GLenum name, GLuint index);


GPU_GL_IMPORT(PFNGLACTIVETEXTUREPROC, glActiveTexture);
GPU_GL_IMPORT(PFNGLATTACHSHADERPROC, glAttachShader);
GPU_GL_IMPORT(PFNGLBINDBUFFERPROC, glBindBuffer);
GPU_GL_IMPORT(PFNGLBINDBUFFERRANGEPROC, glBindBufferRange);
GPU_GL_IMPORT(PFNGLBINDBUFFERBASEPROC, glBindBufferBase);
GPU_GL_IMPORT(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
GPU_GL_IMPORT(PFNGLBINDTEXTURESPROC, glBindTextures);
GPU_GL_IMPORT(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
GPU_GL_IMPORT(PFNGLBINDVERTEXBUFFERPROC, glBindVertexBuffer);
GPU_GL_IMPORT(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate);
GPU_GL_IMPORT(PFNGLBUFFERDATAPROC, glBufferData);
GPU_GL_IMPORT(PFNGLBUFFERSUBDATAPROC, glBufferSubData);
GPU_GL_IMPORT(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus);
GPU_GL_IMPORT(PFNGLCLIENTWAITSYNCPROC, glClientWaitSync);
GPU_GL_IMPORT(PFNGLCLIPCONTROLPROC, glClipControl);
GPU_GL_IMPORT(PFNGLCOMPILESHADERPROC, glCompileShader);
GPU_GL_IMPORT(PFNGLCOMPRESSEDTEXIMAGE2DPROC, glCompressedTexImage2D);
GPU_GL_IMPORT(PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC, glCompressedTexSubImage2D);
GPU_GL_IMPORT(PFNGLCOMPRESSEDTEXTURESUBIMAGE2DPROC, glCompressedTextureSubImage2D);
GPU_GL_IMPORT(PFNGLCOMPRESSEDTEXIMAGE3DPROC, glCompressedTexImage3D);
GPU_GL_IMPORT(PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC, glCompressedTexSubImage3D);
GPU_GL_IMPORT(PFNGLCOMPRESSEDTEXTURESUBIMAGE3DPROC, glCompressedTextureSubImage3D);
GPU_GL_IMPORT(PFNGLCOPYIMAGESUBDATAPROC, glCopyImageSubData);
GPU_GL_IMPORT(PFNGLCOPYNAMEDBUFFERSUBDATAPROC, glCopyNamedBufferSubData);
GPU_GL_IMPORT(PFNGLCREATEBUFFERSPROC, glCreateBuffers);
GPU_GL_IMPORT(PFNGLCREATEFRAMEBUFFERSPROC, glCreateFramebuffers);
GPU_GL_IMPORT(PFNGLCREATEPROGRAMPROC, glCreateProgram);
GPU_GL_IMPORT(PFNGLCREATESHADERPROC, glCreateShader);
GPU_GL_IMPORT(PFNGLCREATETEXTURESPROC, glCreateTextures);
GPU_GL_IMPORT(PFNGLDEBUGMESSAGECALLBACKPROC, glDebugMessageCallback);
GPU_GL_IMPORT(PFNGLDEBUGMESSAGECONTROLPROC, glDebugMessageControl);
GPU_GL_IMPORT(PFNGLDELETEBUFFERSPROC, glDeleteBuffers);
GPU_GL_IMPORT(PFNGLDELETEFRAMEBUFFERSPROC, glDeleteFramebuffers);
GPU_GL_IMPORT(PFNGLDELETEPROGRAMPROC, glDeleteProgram);
GPU_GL_IMPORT(PFNGLDELETEQUERIESPROC, glDeleteQueries);
GPU_GL_IMPORT(PFNGLDELETESHADERPROC, glDeleteShader);
GPU_GL_IMPORT(PFNGLDELETESYNCPROC, glDeleteSync);
GPU_GL_IMPORT(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
GPU_GL_IMPORT(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray);
GPU_GL_IMPORT(PFNGLDISPATCHCOMPUTEPROC, glDispatchCompute);
GPU_GL_IMPORT(PFNGLDRAWARRAYSINSTANCEDARBPROC, glDrawArraysInstanced);
GPU_GL_IMPORT(PFNGLDRAWBUFFERSPROC, glDrawBuffers);
GPU_GL_IMPORT(PFNGLDRAWELEMENTSINSTANCEDPROC, glDrawElementsInstanced);
GPU_GL_IMPORT(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray);
GPU_GL_IMPORT(PFNGLFENCESYNCPROC, glFenceSync);
GPU_GL_IMPORT(PFNGLFLUSHMAPPEDNAMEDBUFFERRANGEPROC, glFlushMappedNamedBufferRange);
GPU_GL_IMPORT(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer);
GPU_GL_IMPORT(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D);
GPU_GL_IMPORT(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers);
GPU_GL_IMPORT(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap);
GPU_GL_IMPORT(PFNGLGENERATETEXTUREMIPMAPPROC, glGenerateTextureMipmap);
GPU_GL_IMPORT(PFNGLGENQUERIESPROC, glGenQueries);
GPU_GL_IMPORT(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers);
GPU_GL_IMPORT(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
GPU_GL_IMPORT(PFNGLGETACTIVEUNIFORMPROC, glGetActiveUniform);
GPU_GL_IMPORT(PFNGLGETDEBUGMESSAGELOGPROC, glGetDebugMessageLog);
GPU_GL_IMPORT(PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC, glGetFramebufferAttachmentParameteriv);
GPU_GL_IMPORT(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog);
GPU_GL_IMPORT(PFNGLGETPROGRAMIVPROC, glGetProgramiv);
GPU_GL_IMPORT(PFNGLGETQUERYOBJECTUI64VPROC, glGetQueryObjectui64v);
GPU_GL_IMPORT(PFNGLGETQUERYOBJECTUIVPROC, glGetQueryObjectuiv);
GPU_GL_IMPORT(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog);
GPU_GL_IMPORT(PFNGLGETSHADERIVPROC, glGetShaderiv);
GPU_GL_IMPORT(PFNGLGETSTRINGIPROC, glGetStringi);
GPU_GL_IMPORT(PFNGLGETTEXTUREIMAGEPROC, glGetTextureImage);
GPU_GL_IMPORT(PFNGLGETTEXTURELEVELPARAMETERIVPROC, glGetTextureLevelParameteriv);
GPU_GL_IMPORT(PFNGLGETUNIFORMBLOCKINDEXPROC, glGetUniformBlockIndex);
GPU_GL_IMPORT(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
GPU_GL_IMPORT(PFNGLLINKPROGRAMPROC, glLinkProgram);
GPU_GL_IMPORT(PFNGLMAPNAMEDBUFFERRANGEPROC, glMapNamedBufferRange);
GPU_GL_IMPORT(PFNGLMEMORYBARRIERPROC, glMemoryBarrier);
GPU_GL_IMPORT(PFNGLMULTIDRAWELEMENTSINDIRECTPROC, glMultiDrawElementsIndirect);
GPU_GL_IMPORT(PFNGLNAMEDBUFFERSTORAGEPROC, glNamedBufferStorage);
GPU_GL_IMPORT(PFNGLNAMEDBUFFERSUBDATAPROC, glNamedBufferSubData);
GPU_GL_IMPORT(PFNGLNAMEDFRAMEBUFFERRENDERBUFFERPROC, glNamedFramebufferRenderbuffer);
GPU_GL_IMPORT(PFNGLNAMEDFRAMEBUFFERTEXTURELAYERPROC, glNamedFramebufferTextureLayer);
GPU_GL_IMPORT(PFNGLNAMEDFRAMEBUFFERTEXTUREPROC, glNamedFramebufferTexture);
GPU_GL_IMPORT(PFNGLOBJECTLABELPROC, glObjectLabel);
GPU_GL_IMPORT(PFNGLPOPDEBUGGROUPPROC, glPopDebugGroup);
GPU_GL_IMPORT(PFNGLPUSHDEBUGGROUPPROC, glPushDebugGroup);
GPU_GL_IMPORT(PFNGLQUERYCOUNTERPROC, glQueryCounter);
GPU_GL_IMPORT(PFNGLSHADERSOURCEPROC, glShaderSource);
GPU_GL_IMPORT(PFNGLTEXBUFFERPROC, glTexBuffer);
GPU_GL_IMPORT(PFNGLTEXIMAGE3DPROC, glTexImage3D);
GPU_GL_IMPORT(PFNGLTEXSUBIMAGE3DPROC, glTexSubImage3D);
GPU_GL_IMPORT(PFNGLTEXTURESUBIMAGE2DPROC, glTextureSubImage2D);
GPU_GL_IMPORT(PFNGLTEXTURESUBIMAGE3DPROC, glTextureSubImage3D);
GPU_GL_IMPORT(PFNGLTEXTUREPARAMETERIPROC, glTextureParameteri);
GPU_GL_IMPORT(PFNGLTEXTURESTORAGE2DPROC, glTextureStorage2D);
GPU_GL_IMPORT(PFNGLTEXTURESTORAGE3DPROC, glTextureStorage3D);
GPU_GL_IMPORT(PFNGLTEXTUREVIEWPROC, glTextureView);
GPU_GL_IMPORT(PFNGLUNIFORM1IPROC, glUniform1i);
GPU_GL_IMPORT(PFNGLUNIFORM2IVPROC, glUniform2iv);
GPU_GL_IMPORT(PFNGLUNIFORM4IVPROC, glUniform4iv);
GPU_GL_IMPORT(PFNGLUNIFORM1FVPROC, glUniform1fv);
GPU_GL_IMPORT(PFNGLUNIFORM2FVPROC, glUniform2fv);
GPU_GL_IMPORT(PFNGLUNIFORM3FVPROC, glUniform3fv);
GPU_GL_IMPORT(PFNGLUNIFORM4FVPROC, glUniform4fv);
GPU_GL_IMPORT(PFNGLUNIFORMMATRIX3X4FVPROC, glUniformMatrix3x4fv);
GPU_GL_IMPORT(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv);
GPU_GL_IMPORT(PFNGLUNIFORMMATRIX4X3FVPROC, glUniformMatrix4x3fv);
GPU_GL_IMPORT(PFNGLUNMAPBUFFERPROC, glUnmapBuffer);
GPU_GL_IMPORT(PFNGLUNMAPNAMEDBUFFERPROC, glUnmapNamedBuffer);
GPU_GL_IMPORT(PFNGLUSEPROGRAMPROC, glUseProgram);
GPU_GL_IMPORT(PFNGLVERTEXATTRIBBINDINGPROC, glVertexAttribBinding);
GPU_GL_IMPORT(PFNGLVERTEXATTRIBFORMATPROC, glVertexAttribFormat);
GPU_GL_IMPORT(PFNGLVERTEXATTRIBIFORMATPROC, glVertexAttribIFormat);
GPU_GL_IMPORT(PFNGLVERTEXATTRIBDIVISORARBPROC, glVertexAttribDivisor);
GPU_GL_IMPORT(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer);
GPU_GL_IMPORT(PFNGLVERTEXBINDINGDIVISORPROC, glVertexBindingDivisor);
