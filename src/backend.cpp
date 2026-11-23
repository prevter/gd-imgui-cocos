#include <Geode/Geode.hpp>
#include <imgui-cocos.hpp>
#include <imgui_internal.h>
#include <imgui.h>
#include <utility>

#ifdef GEODE_IS_WINDOWS
	// so msvc shuts up
	#define sscanf sscanf_s
#endif

using namespace geode::prelude;

struct GLCaps {
	int version = 210;
	bool hasVAO = false;
	bool hasBaseVertex = false;
};

static GLCaps s_caps;
static GLuint s_vao = 0;
static GLuint s_vbo = 0;
static GLuint s_ibo = 0;

static constexpr GLenum IDX_TYPE = sizeof(ImDrawIdx) == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;

static bool hasExtension(std::string_view ext) {
	auto* exts = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS));
	if (!exts) return false;
	std::string_view sv{exts};

	for (size_t pos = 0; (pos = sv.find(ext, pos)) != std::string_view::npos; pos += ext.size()) {
		size_t end = pos + ext.size();
		if ((pos == 0 || sv[pos - 1] == ' ') && (end == sv.size() || sv[end] == ' '))
			return true;
	}

	return false;
}

static int queryGLVersion() {
	int major = 0;
	int minor = 0;
#ifdef GEODE_IS_WINDOWS
	// macos opengl is really outdated, and doesnt have these enums
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	glGetError();
#endif
	if (major == 0 && minor == 0) {
		auto* p = reinterpret_cast<char const*>(glGetString(GL_VERSION));
		if (p && std::strncmp(p, "OpenGL ES", 9) == 0) {
			p += 9;
			while (*p && !(*p >= '0' && *p <= '9')) ++p;
		}

		if (!p || sscanf(p, "%d.%d", &major, &minor) != 2) {
			// failed to parse version string, just assume opengl 2.1 / ES 2.0
			GEODE_MOBILE(return 200);
			return 210;
		}
	}

	return major * 100 + minor * 10;
}

static GLCaps detectCaps() {
	GLCaps caps;
	caps.version = queryGLVersion();

#ifndef GEODE_IS_ANDROID
	#if defined(GEODE_IS_MACOS) || defined(GEODE_IS_IOS)
	caps.hasVAO = hasExtension("GL_APPLE_vertex_array_object");
	#else
	caps.hasVAO = caps.version >= 300 || hasExtension("GL_ARB_vertex_array_object");
	#endif

	#if defined(GEODE_IS_WINDOWS)
	caps.hasVAO = caps.hasVAO && glGenVertexArrays && glBindVertexArray && glDeleteVertexArrays;
	#endif
#endif

#ifndef GEODE_IS_MOBILE
	caps.hasBaseVertex = caps.hasVAO && caps.version >= 320;
	#if defined(GEODE_IS_WINDOWS)
	caps.hasBaseVertex = caps.hasBaseVertex && glDrawElementsBaseVertex;
	#endif
#endif

	return caps;
}

static void destroyGLObjects() {
	if (s_vao) glDeleteVertexArrays(1, &s_vao);
	if (s_vbo) glDeleteBuffers(1, &s_vbo);
	if (s_ibo) glDeleteBuffers(1, &s_ibo);
	s_vao = s_vbo = s_ibo = 0;
}

// little helper function to convert ImTexture2D <=> GLuint,
// supporting both versions of imgui where this was a void* and is now a u64

static GLuint toGLTexture(ImTextureID tex) {
#if IMGUI_VERSION_NUM >= 19140
	return static_cast<GLuint>(tex);
#else
	return static_cast<GLuint>(reinterpret_cast<std::uintptr_t>(tex));
#endif
}
static ImTextureID fromGLTexture(GLuint tex) {
#if IMGUI_VERSION_NUM >= 19140
	return static_cast<ImTextureID>(tex);
#else
	return reinterpret_cast<ImTextureID>(tex);
#endif
}

// make sure this doesn't break in some future version
#if defined(GEODE_IS_WINDOWS) && GEODE_COMP_GD_VERSION >= 22060

#define MAT_SUPPORTS_CURSOR

// we dont have easy access to any of the glfw methods,
// so just directly access the glfwWindow struct, thus making this windows 2.206 only, for now
struct GLFWCursorData {
	void* next = nullptr;
	HCURSOR cursor;
};

static void setMouseCursor(ImGuiMouseCursor cursor) {
	auto* glfwWindow = CCEGLView::get()->getWindow();

	auto& cursorField = *reinterpret_cast<GLFWCursorData**>(reinterpret_cast<uintptr_t>(glfwWindow) + 0x50);
	auto winCursor = IDC_ARROW;
	switch (cursor) {
		case ImGuiMouseCursor_Arrow: winCursor = IDC_ARROW; break;
		case ImGuiMouseCursor_TextInput: winCursor = IDC_IBEAM; break;
		case ImGuiMouseCursor_ResizeAll: winCursor = IDC_SIZEALL; break;
		case ImGuiMouseCursor_ResizeEW: winCursor = IDC_SIZEWE; break;
		case ImGuiMouseCursor_ResizeNS: winCursor = IDC_SIZENS; break;
		case ImGuiMouseCursor_ResizeNESW: winCursor = IDC_SIZENESW; break;
		case ImGuiMouseCursor_ResizeNWSE: winCursor = IDC_SIZENWSE; break;
		case ImGuiMouseCursor_Hand: winCursor = IDC_HAND; break;
		case ImGuiMouseCursor_NotAllowed: winCursor = IDC_NO; break;
	}
	if (cursorField) {
		cursorField->cursor = LoadCursor(NULL, winCursor);
	} else {
		// must be heap allocated
		cursorField = new GLFWCursorData {
			.next = nullptr,
			.cursor = LoadCursor(NULL, winCursor)
		};
	}
}
#endif

#ifdef GEODE_IS_MOBILE

class ImGuiIMEDelegate : public CCIMEDelegate {
protected:
	bool m_attached = false;
	std::string m_text;
public:
	bool attachWithIME() override {
		if (CCIMEDelegate::attachWithIME()) {
			m_attached = true;
			CCEGLView::get()->setIMEKeyboardState(true);
			return true;
		}
		return false;
	}

	bool detachWithIME() override {
		if (CCIMEDelegate::detachWithIME()) {
			m_attached = false;
			CCEGLView::get()->setIMEKeyboardState(false);
			ImGui::ClearActiveID();
			return true;
		}
		return false;
	}

	bool canAttachWithIME() override {
		return true;
	}

	bool canDetachWithIME() override {
		return true;
	}

	char const* getContentText() override {
		auto& buffer = ImGui::GetInputTextState(ImGui::GetFocusID())->TextA;
		m_text = std::string(buffer.begin(), buffer.end());
		return m_text.c_str();
	}

	bool isAttached() {
		return m_attached;
	}

	static ImGuiIMEDelegate* get() {
		static ImGuiIMEDelegate* instance = new ImGuiIMEDelegate();
		return instance;
	}
};

#endif

ImGuiCocos& ImGuiCocos::get() {
	static ImGuiCocos inst;
	return inst;
}

ImGuiCocos::ImGuiCocos() {
	m_setupCall = [] {};
	m_drawCall = [] {};
}

ImGuiCocos& ImGuiCocos::setup(geode::Function<void()> fun) {
	m_setupCall = std::move(fun);
	return this->setup();
}

ImGuiCocos& ImGuiCocos::draw(geode::Function<void()> fun) {
	m_drawCall = std::move(fun);
	return *this;
}

ImGuiCocos& ImGuiCocos::setPreDrawCall(geode::Function<void()> fun) {
	m_preDrawCall = std::move(fun);
	return *this;
}

void ImGuiCocos::runPreDrawCall() {
	if (m_preDrawCall) {
		m_preDrawCall();
	}
}

void ImGuiCocos::toggle() {
	this->setVisible(!m_visible);
}

void ImGuiCocos::setVisible(bool v) {
	m_visible = v;
	if (!m_initialized) return;

	auto& io = ImGui::GetIO();
	if (!m_visible) {
		io.WantCaptureKeyboard = false;
		io.WantCaptureMouse = false;
		io.WantTextInput = false;
		io.ClearInputKeys();
		io.ClearInputMouse();

#ifdef MAT_SUPPORTS_CURSOR
		setMouseCursor(ImGuiMouseCursor_Arrow);
		m_lastCursor = ImGuiMouseCursor_COUNT;
#endif
	}
	io.SetAppAcceptingEvents(m_visible);
}

bool ImGuiCocos::isVisible() const {
	return m_visible;
}

void ImGuiCocos::setInputMode(InputMode mode) {
	m_inputMode = mode;
}

ImGuiCocos::InputMode ImGuiCocos::getInputMode() {
	return m_inputMode;
}

void ImGuiCocos::setForceLegacy(bool force) {
	m_forceLegacy = force;
}

bool ImGuiCocos::getForceLegacy() const {
	return m_forceLegacy;
}

bool ImGuiCocos::isInitialized() const {
	return m_initialized;
}

ImGuiCocos& ImGuiCocos::setup() {
	if (m_initialized) return *this;

	ImGui::CreateContext();

	auto& io = ImGui::GetIO();

	s_caps = detectCaps();

	io.BackendPlatformName = "gd-imgui-cocos + Geode";
	io.BackendPlatformUserData = this;
	if (s_caps.hasBaseVertex) {
		io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
	}
#ifdef IMGUI_HAS_TEXTURES
	io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
#endif

	// use static since imgui does not own the pointer!
	static const auto iniPath = (Mod::get()->getSaveDir() / "imgui.ini").string();
	io.IniFilename = iniPath.c_str();

#if IMGUI_VERSION_NUM >= 19110
	// define geode's clipboard funcs for imgui
	ImGui::GetPlatformIO().Platform_GetClipboardTextFn = [](ImGuiContext* ctx) {
		static std::string text;
		text = geode::utils::clipboard::read();
		return text.c_str();
	};
	ImGui::GetPlatformIO().Platform_SetClipboardTextFn = [](ImGuiContext* ctx, const char* text) {
		geode::utils::clipboard::write(text);
	};
#endif

	io.SetAppAcceptingEvents(m_visible);
	m_lastCursor = ImGuiMouseCursor_COUNT;

	m_initialized = true;

	m_setupCall();

#ifndef IMGUI_HAS_TEXTURES
	unsigned char* pixels;
	int width, height;
	io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

	m_fontTexture = new CCTexture2D;
	m_fontTexture->initWithData(pixels, kCCTexture2DPixelFormat_RGBA8888, width, height, CCSize(static_cast<float>(width), static_cast<float>(height)));

	io.Fonts->SetTexID(fromGLTexture(m_fontTexture->getName()));
#endif

	return *this;
}

void ImGuiCocos::destroy() {
	if (!m_initialized) return;
	destroyGLObjects();

	ImGui::GetIO().BackendPlatformUserData = nullptr;
#ifdef IMGUI_HAS_TEXTURES
	for (auto* tex : ImGui::GetPlatformIO().Textures) {
		if (tex->RefCount == 1) {
			tex->SetStatus(ImTextureStatus_WantDestroy);
			this->updateTexture(tex);
		}
	}
#else
	delete m_fontTexture;
#endif
	ImGui::DestroyContext();
	m_initialized = false;
}

void ImGuiCocos::reload() {
	m_reloading = true;
}

ImVec2 ImGuiCocos::cocosToFrame(const CCPoint& pos) {
	auto* director = CCDirector::sharedDirector();
	const auto frameSize = director->getOpenGLView()->getFrameSize() * geode::utils::getDisplayFactor();
	const auto winSize = director->getWinSize();

	return {
		pos.x / winSize.width * frameSize.width,
		(1.f - pos.y / winSize.height) * frameSize.height
	};
}

CCPoint ImGuiCocos::frameToCocos(const ImVec2& pos) {
	auto* director = CCDirector::sharedDirector();
	const auto frameSize = director->getOpenGLView()->getFrameSize() * geode::utils::getDisplayFactor();
	const auto winSize = director->getWinSize();

	return {
		pos.x / frameSize.width * winSize.width,
		(1.f - pos.y / frameSize.height) * winSize.height
	};
}

void ImGuiCocos::drawFrame() {
	if (!m_initialized || !m_visible) return;

	ccGLBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// starts a new frame for imgui
	this->newFrame();
	ImGui::NewFrame();

	// actually draws stuff with imgui functions
	m_drawCall();

	// renders the triangles onto the screen
	ImGui::Render();
	this->renderFrame();

	// reload imgui context if requested
	if (m_reloading) {
		this->destroy();
		this->setup();
		m_reloading = false;
	}
}

void ImGuiCocos::newFrame() {
	auto& io = ImGui::GetIO();

	// opengl2 new frame
	auto* director = CCDirector::sharedDirector();
	const auto winSize = director->getWinSize();
	const auto frameSize = director->getOpenGLView()->getFrameSize() * geode::utils::getDisplayFactor();

	// glfw new frame
	io.DisplaySize = ImVec2(frameSize.width, frameSize.height);
	if (director->getDeltaTime() > 0.f) {
		io.DeltaTime = director->getDeltaTime();
	} else {
		io.DeltaTime = 1.f / 60.f;
	}

	if (auto pos = geode::cocos::getMousePos(); !pos.isZero()) {
		const auto mouse = cocosToFrame(pos);
		io.AddMouseSourceEvent(ImGuiMouseSource_Mouse);
		io.AddMousePosEvent(mouse.x, mouse.y);
	}

	auto* kb = director->getKeyboardDispatcher();
	io.KeyAlt = kb->getAltKeyPressed() || kb->getCommandKeyPressed(); // look
	io.KeyCtrl = kb->getControlKeyPressed();
	io.KeyShift = kb->getShiftKeyPressed();

#ifdef GEODE_IS_MOBILE
	auto ime = ImGuiIMEDelegate::get();
	if (io.WantTextInput && !ime->isAttached()) {
		ime->attachWithIME();
	} else if (!io.WantTextInput && ime->isAttached()) {
		ime->detachWithIME();
	}
#endif

#ifdef MAT_SUPPORTS_CURSOR
	auto cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
	if (cursor != m_lastCursor) {
		m_lastCursor = cursor;
		setMouseCursor(cursor);
	}
#endif
}

struct FrameTransform {
	CCEGLView* view;
	float scaleX, scaleY, winHeight;

	static FrameTransform current() {
		auto* director = CCDirector::get();
		auto* view = director->getOpenGLView();
		auto frameSize = view->getFrameSize() * utils::getDisplayFactor();
		auto winSize = director->getWinSize();
		return {
			view,
			winSize.width / frameSize.width,
			winSize.height / frameSize.height,
			winSize.height
		};
	}

	bool applyScissor(ImVec4 const& clip) {
		float x0 = clip.x * scaleX;
		float x1 = clip.z * scaleX;
		float yTop = winHeight - clip.y * scaleY;
		float yBottom = winHeight - clip.w * scaleY;
		if (x1 <= x0 || yTop <= yBottom)
			return false;

		view->setScissorInPoints(x0, yBottom, x1 - x0, yTop - yBottom);
		return true;
	}
};

struct ScopedImGuiMatrix {
	explicit ScopedImGuiMatrix(FrameTransform const& tf) {
		kmGLMatrixMode(KM_GL_MODELVIEW);
		kmGLPushMatrix();
		kmGLLoadIdentity();
		kmGLTranslatef(0.f, tf.winHeight, 0.f);
		kmGLScalef(tf.scaleX, -tf.scaleY, 1.f);
	}

	~ScopedImGuiMatrix() {
		kmGLMatrixMode(KM_GL_MODELVIEW);
		kmGLPopMatrix();
	}

	ScopedImGuiMatrix(ScopedImGuiMatrix const&) = delete;
	ScopedImGuiMatrix& operator=(ScopedImGuiMatrix const&) = delete;
};

static void useImGuiShader() {
	auto* shader = CCShaderCache::sharedShaderCache()->programForKey(kCCShader_PositionTextureColor);
	shader->use();
	shader->setUniformsForBuiltins();
}

void ImGuiCocos::legacyRenderFrame() const {
	auto* drawData = ImGui::GetDrawData();
	auto tf = FrameTransform::current();

	glEnable(GL_SCISSOR_TEST);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	ScopedImGuiMatrix matrix(tf);
	auto setupState = [] {
		useImGuiShader();
		ccGLEnableVertexAttribs(kCCVertexAttribFlag_PosColorTex);
	};
	setupState();

	for (int i = 0; i < drawData->CmdListsCount; ++i) {
		auto* list = drawData->CmdLists[i];
		auto* vtx = list->VtxBuffer.Data;
		auto* idx = list->IdxBuffer.Data;

		for (auto& cmd : list->CmdBuffer) {
			if (cmd.UserCallback != nullptr) {
				if (cmd.UserCallback == ImDrawCallback_ResetRenderState) {
					setupState();
				} else {
					cmd.UserCallback(list, &cmd);
				}
				continue;
			}

			if (cmd.ElemCount == 0 || !tf.applyScissor(cmd.ClipRect)) continue;

			ccGLBindTexture2D(toGLTexture(cmd.GetTexID()));

			auto* base = reinterpret_cast<char const*>(vtx + cmd.VtxOffset);
			glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), base + offsetof(ImDrawVert, pos));
			glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), base + offsetof(ImDrawVert, uv));
			glVertexAttribPointer(kCCVertexAttrib_Color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), base + offsetof(ImDrawVert, col));

			glDrawElements(GL_TRIANGLES, cmd.ElemCount, IDX_TYPE, idx + cmd.IdxOffset);
		}
	}

	glDisable(GL_SCISSOR_TEST);
}

void ImGuiCocos::renderFrame() const {
	auto* drawData = ImGui::GetDrawData();
	if (!drawData) return;

#ifdef IMGUI_HAS_TEXTURES
	if (drawData->Textures != nullptr) {
		for (auto* tex : *drawData->Textures) {
			if (tex->Status != ImTextureStatus_OK) {
				this->updateTexture(tex);
			}
		}
	}
#endif

	if (drawData->CmdListsCount == 0 || drawData->DisplaySize.x <= 0.f || drawData->DisplaySize.y <= 0.f)
		return;

	if (!s_caps.hasVAO || m_forceLegacy)
		return legacyRenderFrame();

	bool useBaseVertex = s_caps.hasBaseVertex;
	auto tf = FrameTransform::current();

	glEnable(GL_SCISSOR_TEST);

	if (s_vao == 0) {
		glGenVertexArrays(1, &s_vao);
		glGenBuffers(1, &s_vbo);
		glGenBuffers(1, &s_ibo);

		glBindVertexArray(s_vao);
		glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_ibo);

		glEnableVertexAttribArray(kCCVertexAttrib_Position);
		glVertexAttribPointer(kCCVertexAttrib_Position, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, pos)));
		glEnableVertexAttribArray(kCCVertexAttrib_TexCoords);
		glVertexAttribPointer(kCCVertexAttrib_TexCoords, 2, GL_FLOAT, GL_FALSE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, uv)));
		glEnableVertexAttribArray(kCCVertexAttrib_Color);
		glVertexAttribPointer(kCCVertexAttrib_Color, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(ImDrawVert), reinterpret_cast<void*>(offsetof(ImDrawVert, col)));
	}

	ScopedImGuiMatrix matrix(tf);
	auto setupState = [] {
		glBindVertexArray(s_vao);
		glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
		useImGuiShader();
	};
	setupState();

	for (int i = 0; i < drawData->CmdListsCount; ++i) {
		auto* list = drawData->CmdLists[i];

		glBufferData(GL_ARRAY_BUFFER, list->VtxBuffer.Size * sizeof(ImDrawVert), list->VtxBuffer.Data, GL_STREAM_DRAW);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, list->IdxBuffer.Size * sizeof(ImDrawIdx), list->IdxBuffer.Data, GL_STREAM_DRAW);

		for (auto& cmd : list->CmdBuffer) {
			if (cmd.UserCallback != nullptr) {
				if (cmd.UserCallback == ImDrawCallback_ResetRenderState) {
					setupState();
				} else {
					cmd.UserCallback(list, &cmd);
				}
				continue;
			}

			if (cmd.ElemCount == 0 || !tf.applyScissor(cmd.ClipRect))
				continue;

			ccGLBindTexture2D(toGLTexture(cmd.GetTexID()));

			auto* idxOffset = reinterpret_cast<void*>(static_cast<uintptr_t>(cmd.IdxOffset) * sizeof(ImDrawIdx));

			if (useBaseVertex) {
			#if !defined(GEODE_IS_MOBILE)
				glDrawElementsBaseVertex(GL_TRIANGLES, cmd.ElemCount, IDX_TYPE, idxOffset, static_cast<GLint>(cmd.VtxOffset));
			#endif
			} else {
				glDrawElements(GL_TRIANGLES, cmd.ElemCount, IDX_TYPE, idxOffset);
			}
		}
	}

	glBindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

	glDisable(GL_SCISSOR_TEST);
}

#ifdef IMGUI_HAS_TEXTURES
void ImGuiCocos::updateTexture(ImTextureData* tex) const {
	if (tex->Status == ImTextureStatus_WantCreate) {
		IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
		const void* pixels = tex->GetPixels();
		auto* ccTexture = new CCTexture2D;
		ccTexture->initWithData(pixels, kCCTexture2DPixelFormat_RGBA8888, tex->Width, tex->Height, CCSize(static_cast<float>(tex->Width), static_cast<float>(tex->Height)));

		tex->SetTexID(fromGLTexture(ccTexture->getName()));
		tex->BackendUserData = ccTexture;
		tex->SetStatus(ImTextureStatus_OK);
	} else if (tex->Status == ImTextureStatus_WantUpdates) {
		// cocos has no function for this, do it manually with opengl
		// copied from `imgui_impl_opengl3.cpp`
	#ifdef GL_UNPACK_ALIGNMENT
		// dont mess up odd widths
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	#endif

		GLint lastTexture;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
		GLuint texID = toGLTexture(tex->TexID);
		glBindTexture(GL_TEXTURE_2D, texID);

	#ifdef GL_UNPACK_ROW_LENGTH
		glPixelStorei(GL_UNPACK_ROW_LENGTH, tex->Width);
		for (ImTextureRect& r : tex->Updates) {
			glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, tex->GetPixelsAt(r.x, r.y));
		}
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	#else
		// GL ES doesn't have GL_UNPACK_ROW_LENGTH, so copy to a contiguous buffer first
		static std::vector<unsigned char> tempBuffer;
		for (ImTextureRect& r : tex->Updates) {
			tempBuffer.resize(r.w * r.h * tex->BytesPerPixel);
			auto* ptr = tempBuffer.data();
			int dataWidth = r.w * tex->BytesPerPixel;
			for (int y = 0; y < r.h; y++) {
				std::memcpy(ptr, tex->GetPixelsAt(r.x, r.y + y), dataWidth);
				ptr += dataWidth;
			}
			glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE, tempBuffer.data());
		}
	#endif

		glBindTexture(GL_TEXTURE_2D, lastTexture); // Restore state

		tex->SetStatus(ImTextureStatus_OK);
	} else if (tex->Status == ImTextureStatus_WantDestroy) {
		auto* ccTexture = static_cast<CCTexture2D*>(tex->BackendUserData);
		delete ccTexture;

		tex->SetTexID(ImTextureID_Invalid);
		tex->BackendUserData = nullptr;
		tex->SetStatus(ImTextureStatus_Destroyed);
	}
}
#endif
