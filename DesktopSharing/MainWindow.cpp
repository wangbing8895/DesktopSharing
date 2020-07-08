#include "MainWindow.h"
#include <mutex>

MainWindow::MainWindow()
{
	window_width_   = 960;
	window_height_  = 740;
	video_width_    = window_width_;
	video_height_   = window_height_ - kOverlayHeight;
	overlay_width_  = window_width_;
	overlay_height_ = kOverlayHeight;

	avconfig_.bitrate_bps = 4000000; // video bitrate
	avconfig_.framerate = 25;        // video framerate
	avconfig_.codec = "";  // hardware encoder: "h264_nvenc";    
}

MainWindow::~MainWindow()
{

}

bool MainWindow::Create()
{
	static std::once_flag init_flag;
	std::call_once(init_flag, [=]() {
		SDL_assert(SDL_Init(SDL_INIT_EVERYTHING) == 0);
	});

	int window_flag = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE;
	window_ = SDL_CreateWindow("Screen Live", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		window_width_, window_height_, window_flag);

	SDL_SysWMinfo wm_info;
	SDL_VERSION(&wm_info.version);
	if (SDL_GetWindowWMInfo(window_, &wm_info)) {
		window_handle_ = wm_info.info.win.window;
	}

	if (!InitD3D()) {
		Destroy();
		return false;
	}

	/* disable chinese input */
	if (window_handle_) {
		ImmAssociateContext((HWND)window_handle_, nullptr);
	}

	SDL_SetWindowMinimumSize(window_, window_width_, window_height_);

	return (window_handle_ && window_);
}

void MainWindow::Destroy()
{
	if (window_) {
		ClearD3D();
		SDL_DestroyWindow(window_);
		window_ = nullptr;
		window_handle_ = nullptr;
		//SDL_Quit();
	}
}

bool MainWindow::IsWindow() const
{
	return window_ != nullptr;
}

void MainWindow::Resize()
{
	if (IsWindow()) {
		int width = 0, height = 0;
		SDL_GetWindowSize(window_, &width, &height);

		window_width_ = width;
		window_height_ = height;
		video_width_ = window_width_;
		video_height_ = window_height_ - kOverlayHeight;
		overlay_width_ = window_width_;
		overlay_height_ = kOverlayHeight;

		ClearD3D();
		InitD3D();
	}
}

bool MainWindow::InitD3D()
{
	int driver_index = -1;
	int driver_count = SDL_GetNumRenderDrivers();
	int renderer_flags = SDL_RENDERER_SOFTWARE;

	for (int i = 0; i < driver_count; i++) {
		SDL_RendererInfo info;
		if (SDL_GetRenderDriverInfo(i, &info) < 0) {
			continue;
		}

		if (strcmp(info.name, "direct3d") == 0) {
			driver_index = i;
			if (info.flags & SDL_RENDERER_ACCELERATED) {
				renderer_flags = SDL_RENDERER_ACCELERATED;
			}
		}
	}

	if (driver_index < 0) {
		return false;
	}

	renderer_ = SDL_CreateRenderer(window_, driver_index, renderer_flags);
	SDL_assert(renderer_ != nullptr);

	device_ = SDL_RenderGetD3D9Device(renderer_);
	SDL_assert(device_ != nullptr);

	SDL_SetRenderDrawColor(renderer_, 114, 144, 154, SDL_ALPHA_OPAQUE);
	SDL_RenderClear(renderer_);
	SDL_RenderPresent(renderer_);

	overlay_ = new Overlay;
	if (!overlay_->Init(window_, device_)) {
		delete overlay_;
		overlay_ = nullptr;
	}
	overlay_->SetRect(0, 0 + video_height_, video_width_, kOverlayHeight);
	overlay_->RegisterObserver(this);
	return true;
}

void MainWindow::ClearD3D()
{
	if (overlay_) {
		overlay_->Destroy();
		delete overlay_;
		overlay_ = nullptr;
	}

	if (texture_) {
		SDL_DestroyTexture(texture_);
		texture_ = nullptr;
		texture_format_ = SDL_PIXELFORMAT_UNKNOWN;
		texture_width_ = 0;
		texture_height_ = 0;
	}

	if (renderer_) {
		SDL_DestroyRenderer(renderer_);
		renderer_ = nullptr;
	}
}

void MainWindow::Porcess(SDL_Event& event)
{
	if (IsWindow()) {
		if (overlay_) {
			Overlay::Process(&event);
		}		
	}	
}

bool MainWindow::UpdateARGB(const uint8_t* data, uint32_t width, uint32_t height)
{
	if (!IsWindow()) {
		return false;
	}

	if (texture_format_ != SDL_PIXELFORMAT_ARGB8888 ||
		(texture_width_ != width) || (texture_height_ != height)) {
		if (texture_) {
			SDL_DestroyTexture(texture_);
			texture_ = nullptr;
		}	
	}

	if (!texture_) {		
		texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888,
			SDL_TEXTUREACCESS_STREAMING, width, height);
		SDL_assert(texture_ != nullptr);
		texture_format_ = SDL_PIXELFORMAT_ARGB8888;
		texture_width_ = width;
		texture_height_ = height;
	}

	if (texture_) {
		char* pixels = nullptr;
		int pitch = 0;

		int ret = SDL_LockTexture(texture_, nullptr, (void**)&pixels, &pitch);
		//SDL_assert(ret >= 0);
		if (ret < 0) {
			return false;
		}
		
		memcpy(pixels, data, texture_width_ * texture_height_ * 4);
		SDL_UnlockTexture(texture_);

		//SDL_SetRenderDrawColor(renderer_, 114, 144, 154, SDL_ALPHA_OPAQUE);
		SDL_RenderClear(renderer_);

		SDL_Rect rect = { 0, 0, video_width_, video_height_ };
		SDL_RenderCopy(renderer_, texture_, nullptr, &rect);
		if (overlay_) {
			overlay_->Render();
		}
		SDL_RenderPresent(renderer_);

		return true;
	}

	return false;
}

bool MainWindow::StartLive(int& event_type, 
	std::vector<std::string>& encoder_settings,
	std::vector<std::string>& live_settings)
{
	AVConfig avconfig;
	avconfig.framerate = atoi(encoder_settings[1].c_str());
	avconfig.bitrate_bps = atoi(encoder_settings[2].c_str()) * 1000U;
	avconfig.codec = encoder_settings[0];
	if (avconfig.codec == "h264_nvenc") {
		if (!nvenc_info.is_supported()) {
			avconfig.codec = "h264";
		}
	}

	/* reset video encoder */
	if (avconfig_ != avconfig) {
		ScreenLive::Instance().StopLive(SCREEN_LIVE_RTSP_SERVER);
		ScreenLive::Instance().StopLive(SCREEN_LIVE_RTSP_PUSHER);
		ScreenLive::Instance().StopLive(SCREEN_LIVE_RTMP_PUSHER);
		overlay_->SetLiveState(EVENT_TYPE_RTSP_SERVER, false);
		overlay_->SetLiveState(EVENT_TYPE_RTSP_PUSHER, false);
		overlay_->SetLiveState(EVENT_TYPE_RTMP_PUSHER, false);
		ScreenLive::Instance().StopEncoder();
		if (ScreenLive::Instance().StartEncoder(avconfig) < 0) {
			return false;
		}
		avconfig_ = avconfig;		
	}

	if (!ScreenLive::Instance().IsEncoderInitialized()) {
		return false;
	}

	LiveConfig live_config;
	bool ret = false;

	if (event_type == EVENT_TYPE_RTSP_SERVER) {
		live_config.ip = live_settings[0];
		live_config.port = atoi(live_settings[1].c_str());
		live_config.suffix = live_settings[2];
		ret = ScreenLive::Instance().StartLive(SCREEN_LIVE_RTSP_SERVER, live_config);
	}
	else if (event_type == EVENT_TYPE_RTSP_PUSHER) {
		live_config.rtsp_url = live_settings[0];
		ret = ScreenLive::Instance().StartLive(SCREEN_LIVE_RTSP_PUSHER, live_config);
	}
	else if (event_type == EVENT_TYPE_RTMP_PUSHER) {
		live_config.rtmp_url = live_settings[0];
		ret = ScreenLive::Instance().StartLive(SCREEN_LIVE_RTMP_PUSHER, live_config);
	}

	return ret;
}

void MainWindow::StopLive(int event_type)
{
	if (event_type == EVENT_TYPE_RTSP_SERVER) {
		ScreenLive::Instance().StopLive(SCREEN_LIVE_RTSP_SERVER);
	}
	else if (event_type == EVENT_TYPE_RTSP_PUSHER) {
		ScreenLive::Instance().StopLive(SCREEN_LIVE_RTSP_PUSHER);
	}
	else if (event_type == EVENT_TYPE_RTMP_PUSHER) {
		ScreenLive::Instance().StopLive(SCREEN_LIVE_RTMP_PUSHER);
	}
}