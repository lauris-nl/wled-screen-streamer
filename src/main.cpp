#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using Ns = std::chrono::nanoseconds;
static std::atomic_bool running{true};
static void stop_handler(int) { running.store(false, std::memory_order_relaxed); }

struct Rect { int x{}, y{}, w{}, h{}; };
enum class Filter { nearest, bilinear };
enum class Pattern { none, red, green, blue, solid_blue, checkerboard, line };
enum class CropMode { full, center_square, center_native };
enum class AaMode { off, gaussian, supersample };
enum class Source { screen, rtsp };
enum class RtspTransport { tcp, udp };

struct Options {
  std::string host = "wled-matrix2.local";
  std::string url;
  int width = 64, height = 64;
  double fps = 60.0, aa_strength = 1.0;
  double saturation = 1.15, contrast = 1.10, brightness = 1.00, gamma = 1.00;
  int monitor = 0, crop_x = -1, crop_y = -1, crop_size = -1, supersample = 2;
  bool crop_set = false, benchmark = false, ddp_debug = false, color_correction = true;
  Rect crop{};
  Filter filter = Filter::nearest;
  Pattern pattern = Pattern::none;
  CropMode crop_mode = CropMode::full;
  AaMode aa = AaMode::supersample;
  Source source = Source::screen;
  RtspTransport rtsp_transport = RtspTransport::tcp;
};

[[noreturn]] static void fail(const std::string& s) { throw std::runtime_error(s); }
static int integer(std::string_view s, const char* name) {
  int v{}; auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (e != std::errc{} || p != s.data() + s.size()) fail(std::string("ongeldige ") + name + ": " + std::string(s));
  return v;
}
static double real(std::string_view s, const char* name) {
  char* end{}; std::string t(s); double v = std::strtod(t.c_str(), &end);
  if (!end || *end) fail(std::string("ongeldige ") + name + ": " + t);
  return v;
}
static Rect parse_crop(std::string s) {
  for (char& c : s) if (c == ',' || c == 'x' || c == '+' || c == ':') c = ' ';
  Rect r; char extra; std::istringstream in(s);
  if (!(in >> r.x >> r.y >> r.w >> r.h) || (in >> extra) || r.w <= 0 || r.h <= 0) fail("--crop verwacht X,Y,W,H");
  return r;
}
static void usage() {
  std::cout << "Usage: wled-screen-streamer [options]\n"
    "  --host HOST             WLED host (default wled-matrix2.local)\n"
    "  --source screen|rtsp    Video source (default screen)\n"
    "  --url URL               RTSP URL (required for source rtsp)\n"
    "  --rtsp-transport tcp|udp (default tcp)\n"
    "  --width N --height N    Matrix dimensions (default 64x64)\n"
    "  --fps N                 Target FPS; 0 = uncapped\n"
    "  --monitor N             XRandR monitor index (default 0)\n"
    "  --crop X,Y,W,H          Capture region; overrides --monitor\n"
    "  --crop-mode full|center-square|center-native (default full)\n"
    "  --crop-x N --crop-y N   Manual center-square position\n"
    "  --crop-size N           Manual square crop size\n"
    "  --filter nearest|bilinear\n"
    "  --aa off|gaussian|supersample (default supersample)\n"
    "  --aa-strength FLOAT     Gaussian sigma 0.5..2.0 (default 1.0)\n"
    "  --supersample N         Supersampling factor (default 2)\n"
    "  --saturation FLOAT      Color saturation (default 1.15)\n"
    "  --contrast FLOAT        Contrast around midpoint (default 1.10)\n"
    "  --brightness FLOAT      Brightness factor (default 1.00)\n"
    "  --gamma FLOAT           Streamer gamma (default 1.00 = off)\n"
    "  --no-color-correction   Disable all color correction\n"
    "  --test-pattern red|green|blue|solid-blue|checkerboard|line\n"
    "  --ddp-debug             Print the 9 DDP headers for the first frame\n"
    "  --benchmark             Print timing/CPU statistics every second\n"
    "  --help\n";
}
static Options parse(int argc, char** argv) {
  Options o;
  auto value = [&](int& i) -> std::string_view { if (++i >= argc) fail(std::string("waarde ontbreekt na ") + argv[i-1]); return argv[i]; };
  for (int i = 1; i < argc; ++i) {
    std::string_view a = argv[i];
    if (a == "--help") { usage(); std::exit(0); }
    else if (a == "--host") o.host = value(i);
    else if (a == "--source") {auto v=value(i);if(v=="screen")o.source=Source::screen;else if(v=="rtsp")o.source=Source::rtsp;else fail("source moet screen of rtsp zijn");}
    else if (a == "--url") o.url = value(i);
    else if (a == "--rtsp-transport") {auto v=value(i);if(v=="tcp")o.rtsp_transport=RtspTransport::tcp;else if(v=="udp")o.rtsp_transport=RtspTransport::udp;else fail("rtsp-transport moet tcp of udp zijn");}
    else if (a == "--width") o.width = integer(value(i), "breedte");
    else if (a == "--height") o.height = integer(value(i), "hoogte");
    else if (a == "--fps") o.fps = real(value(i), "fps");
    else if (a == "--monitor") o.monitor = integer(value(i), "monitor");
    else if (a == "--crop") { o.crop = parse_crop(std::string(value(i))); o.crop_set = true; }
    else if (a == "--crop-mode") { auto v=value(i); if(v=="full")o.crop_mode=CropMode::full;else if(v=="center-square")o.crop_mode=CropMode::center_square;else if(v=="center-native")o.crop_mode=CropMode::center_native;else fail("crop-mode moet full, center-square of center-native zijn"); }
    else if (a == "--crop-x") o.crop_x = integer(value(i), "crop-x");
    else if (a == "--crop-y") o.crop_y = integer(value(i), "crop-y");
    else if (a == "--crop-size") o.crop_size = integer(value(i), "crop-size");
    else if (a == "--filter") { auto v=value(i); if(v=="nearest") o.filter=Filter::nearest; else if(v=="bilinear") o.filter=Filter::bilinear; else fail("filter moet nearest of bilinear zijn"); }
    else if (a == "--aa") { auto v=value(i); if(v=="off")o.aa=AaMode::off;else if(v=="gaussian")o.aa=AaMode::gaussian;else if(v=="supersample")o.aa=AaMode::supersample;else fail("aa moet off, gaussian of supersample zijn"); }
    else if (a == "--aa-strength") o.aa_strength = real(value(i), "aa-strength");
    else if (a == "--supersample") o.supersample = integer(value(i), "supersample");
    else if (a == "--saturation") o.saturation = real(value(i), "saturation");
    else if (a == "--contrast") o.contrast = real(value(i), "contrast");
    else if (a == "--brightness") o.brightness = real(value(i), "brightness");
    else if (a == "--gamma") o.gamma = real(value(i), "gamma");
    else if (a == "--no-color-correction") o.color_correction = false;
    else if (a == "--benchmark") o.benchmark = true;
    else if (a == "--ddp-debug") o.ddp_debug = true;
    else if (a == "--test-pattern") {
      auto v=value(i); if(v=="rood"||v=="red") o.pattern=Pattern::red; else if(v=="groen"||v=="green") o.pattern=Pattern::green;
      else if(v=="blauw"||v=="blue") o.pattern=Pattern::blue; else if(v=="solid-blue") o.pattern=Pattern::solid_blue;
      else if(v=="checkerboard") o.pattern=Pattern::checkerboard;
      else if(v=="lijn"||v=="line") o.pattern=Pattern::line; else fail("onbekend testpatroon");
    } else fail("onbekende optie: " + std::string(a));
  }
  if (o.width <= 0 || o.height <= 0 || o.width > 4096 || o.height > 4096 || o.fps < 0 ||
      o.crop_x < -1 || o.crop_y < -1 || o.crop_size == 0 || o.crop_size < -1 || o.supersample<1 || o.supersample>8 ||
      o.aa_strength<0.5 || o.aa_strength>2.0) fail("ongeldige afmetingen, fps, crop of anti-aliasingwaarde");
  if(!std::isfinite(o.saturation)||!std::isfinite(o.contrast)||!std::isfinite(o.brightness)||!std::isfinite(o.gamma)||
      o.saturation<0||o.saturation>4||o.contrast<0||o.contrast>4||o.brightness<0||o.brightness>4||o.gamma<0.1||o.gamma>5)
    fail("kleurwaarden buiten bereik (saturation/contrast/brightness 0..4, gamma 0.1..5)");
  if (static_cast<int64_t>(o.width)*o.supersample>4096 || static_cast<int64_t>(o.height)*o.supersample>4096)
    fail("supersample-werkafmetingen zijn groter dan 4096");
  if ((o.crop_x >= 0 || o.crop_y >= 0 || o.crop_size > 0) && o.crop_mode != CropMode::center_square)
    fail("--crop-x, --crop-y en --crop-size vereisen --crop-mode center-square");
  if(o.source==Source::rtsp&&o.url.empty()&&o.pattern==Pattern::none)fail("--source rtsp vereist --url URL");
  return o;
}

class XCapture {
  Display* d_{}; Window root_{}; XImage* image_{}; XShmSegmentInfo shm_{};
  Rect area_{}, monitor_area_{};
  int monitor_index_{}, crop_x_{}, crop_y_{}, crop_size_{}, output_w_{}, output_h_{}, rr_event_base_{};
  bool crop_set_{};
  Rect explicit_crop_{};
  CropMode crop_mode_{CropMode::full};

  void destroy_image() {
    if (!image_) return;
    XShmDetach(d_,&shm_); XSync(d_,False);
    image_->data=nullptr; XDestroyImage(image_); image_=nullptr;
    if(shm_.shmaddr && shm_.shmaddr != reinterpret_cast<char*>(-1)) shmdt(shm_.shmaddr);
    shm_={};
  }
  void create_image() {
    image_=XShmCreateImage(d_,DefaultVisual(d_,DefaultScreen(d_)),static_cast<unsigned>(DefaultDepth(d_,DefaultScreen(d_))),ZPixmap,nullptr,&shm_,static_cast<unsigned>(area_.w),static_cast<unsigned>(area_.h));
    if(!image_) fail("XShmCreateImage mislukt");
    shm_.shmid=shmget(IPC_PRIVATE,static_cast<size_t>(image_->bytes_per_line)*static_cast<size_t>(image_->height),IPC_CREAT|0600);
    if(shm_.shmid<0) fail("shmget mislukt");
    shm_.shmaddr=image_->data=static_cast<char*>(shmat(shm_.shmid,nullptr,0)); shm_.readOnly=False;
    if(shm_.shmaddr==reinterpret_cast<char*>(-1)) fail("shmat mislukt");
    if(!XShmAttach(d_,&shm_)) fail("XShmAttach mislukt");
    XSync(d_,False); shmctl(shm_.shmid,IPC_RMID,nullptr);
  }
  void configure(bool announce) {
    Rect monitor{};
    if(crop_set_) {
      monitor={0,0,DisplayWidth(d_,DefaultScreen(d_)),DisplayHeight(d_,DefaultScreen(d_))};
    } else {
      int count{}; XRRMonitorInfo* monitors=XRRGetMonitors(d_,root_,True,&count);
      if(!monitors||monitor_index_<0||monitor_index_>=count){if(monitors)XRRFreeMonitors(monitors);fail("ongeldige monitorindex");}
      monitor={monitors[monitor_index_].x,monitors[monitor_index_].y,monitors[monitor_index_].width,monitors[monitor_index_].height};
      XRRFreeMonitors(monitors);
    }
    Rect next{};
    if(crop_set_) next=explicit_crop_;
    else if(crop_mode_==CropMode::full) next=monitor;
    else if(crop_mode_==CropMode::center_native) {
      if(output_w_>monitor.w||output_h_>monitor.h) fail("center-native output is groter dan de geselecteerde monitor");
      next={monitor.x+(monitor.w-output_w_)/2,monitor.y+(monitor.h-output_h_)/2,output_w_,output_h_};
    } else {
      int size=crop_size_>0?crop_size_:std::min(monitor.w,monitor.h);
      int rel_x=crop_x_>=0?crop_x_:(monitor.w-size)/2;
      int rel_y=crop_y_>=0?crop_y_:(monitor.h-size)/2;
      next={monitor.x+rel_x,monitor.y+rel_y,size,size};
    }
    int sw=DisplayWidth(d_,DefaultScreen(d_)),sh=DisplayHeight(d_,DefaultScreen(d_));
    if(next.w<=0||next.h<=0||next.x<0||next.y<0||next.x+next.w>sw||next.y+next.h>sh) fail("berekende crop valt buiten het X11-scherm");
    bool changed=!image_||next.x!=area_.x||next.y!=area_.y||next.w!=area_.w||next.h!=area_.h;
    monitor_area_=monitor;
    if(changed){destroy_image();area_=next;create_image();}
    if(announce||changed) {
      std::cerr<<"Capture: "<<monitor_area_.w<<"x"<<monitor_area_.h<<"\nCrop: ";
      if(crop_set_)std::cerr<<"manual ";else if(crop_mode_==CropMode::center_square)std::cerr<<"center-square ";else if(crop_mode_==CropMode::center_native)std::cerr<<"center-native ";else std::cerr<<"full ";
      std::cerr<<area_.w<<"x"<<area_.h<<"+"<<area_.x<<"+"<<area_.y<<"\n";
    }
  }
 public:
  XCapture(const Options& o):monitor_index_(o.monitor),crop_x_(o.crop_x),crop_y_(o.crop_y),crop_size_(o.crop_size),output_w_(o.width*(o.aa==AaMode::supersample?o.supersample:1)),output_h_(o.height*(o.aa==AaMode::supersample?o.supersample:1)),crop_set_(o.crop_set),explicit_crop_(o.crop),crop_mode_(o.crop_mode) {
    d_ = XOpenDisplay(nullptr); if (!d_) fail("kan X11-display niet openen (controleer DISPLAY/XAUTHORITY)");
    if (!XShmQueryExtension(d_)) fail("X11 MIT-SHM is niet beschikbaar");
    root_ = DefaultRootWindow(d_);
    int rr_error{}; if(!XRRQueryExtension(d_,&rr_event_base_,&rr_error)) fail("XRandR is niet beschikbaar");
    XRRSelectInput(d_,root_,RRScreenChangeNotifyMask|RRCrtcChangeNotifyMask|RROutputChangeNotifyMask);
    configure(true);
  }
  ~XCapture() {
    if(d_) destroy_image();
    if(d_) XCloseDisplay(d_);
  }
  XCapture(const XCapture&)=delete; XCapture& operator=(const XCapture&)=delete;
  void grab() {
    bool reconfigure=false;
    while(XPending(d_)){XEvent event{};XNextEvent(d_,&event);if(event.type==rr_event_base_+RRScreenChangeNotify){XRRUpdateConfiguration(&event);reconfigure=true;}else if(event.type==rr_event_base_+RRNotify)reconfigure=true;}
    if(reconfigure)configure(false);
    if(!XShmGetImage(d_,root_,image_,area_.x,area_.y,AllPlanes)) fail("XShmGetImage mislukt");
  }
  const XImage& image() const { return *image_; }
  const Rect& area() const { return area_; }
};

static inline uint8_t channel(unsigned long pixel, unsigned long mask) {
  if (!mask) return 0;
  unsigned shift=std::countr_zero(mask);
  unsigned long m=mask>>shift, v=(pixel&mask)>>shift;
  return static_cast<uint8_t>((v*255UL + m/2UL)/m);
}
static inline void get_rgb(const XImage& im, int x, int y, uint8_t* p) {
  unsigned long v;
  if (im.bits_per_pixel == 32) { std::memcpy(&v, im.data + static_cast<ptrdiff_t>(y)*im.bytes_per_line + static_cast<ptrdiff_t>(x)*4, 4); }
  else if (im.bits_per_pixel == 24) { const auto* s=reinterpret_cast<const uint8_t*>(im.data + static_cast<ptrdiff_t>(y)*im.bytes_per_line + static_cast<ptrdiff_t>(x)*3); v=static_cast<unsigned long>(s[0])|(static_cast<unsigned long>(s[1])<<8)|(static_cast<unsigned long>(s[2])<<16); }
  else { v=XGetPixel(const_cast<XImage*>(&im),x,y); }
  p[0]=channel(v,im.red_mask); p[1]=channel(v,im.green_mask); p[2]=channel(v,im.blue_mask);
}
static void resize_nearest(const XImage& im, int ow, int oh, std::vector<uint8_t>& out) {
  for(int y=0;y<oh;++y) { int sy=std::min(im.height-1, static_cast<int>((static_cast<int64_t>(2*y+1)*im.height)/(2*oh)));
    for(int x=0;x<ow;++x) { int sx=std::min(im.width-1, static_cast<int>((static_cast<int64_t>(2*x+1)*im.width)/(2*ow))); get_rgb(im,sx,sy,&out[(static_cast<size_t>(y)*ow+x)*3]); }
  }
}
static void resize_bilinear(const XImage& im, int ow, int oh, std::vector<uint8_t>& out) {
  for(int y=0;y<oh;++y) { double fy=(y+.5)*im.height/oh-.5; int y0=std::clamp(static_cast<int>(std::floor(fy)),0,im.height-1), y1=std::min(y0+1,im.height-1); double wy=std::clamp(fy-y0,0.0,1.0);
    for(int x=0;x<ow;++x) { double fx=(x+.5)*im.width/ow-.5; int x0=std::clamp(static_cast<int>(std::floor(fx)),0,im.width-1),x1=std::min(x0+1,im.width-1); double wx=std::clamp(fx-x0,0.0,1.0); uint8_t a[3],b[3],c[3],d[3]; get_rgb(im,x0,y0,a);get_rgb(im,x1,y0,b);get_rgb(im,x0,y1,c);get_rgb(im,x1,y1,d); auto* q=&out[(static_cast<size_t>(y)*ow+x)*3]; for(int k=0;k<3;++k) q[k]=static_cast<uint8_t>(std::lround((1-wy)*((1-wx)*a[k]+wx*b[k])+wy*((1-wx)*c[k]+wx*d[k]))); }
  }
}
struct RgbView { const uint8_t* data{}; int width{},height{},stride{}; Rect crop{}; };
static void resize_rgb_nearest(const RgbView& im,int ow,int oh,std::vector<uint8_t>& out){
  for(int y=0;y<oh;++y){int sy=im.crop.y+std::min(im.crop.h-1,static_cast<int>((static_cast<int64_t>(2*y+1)*im.crop.h)/(2*oh)));
    for(int x=0;x<ow;++x){int sx=im.crop.x+std::min(im.crop.w-1,static_cast<int>((static_cast<int64_t>(2*x+1)*im.crop.w)/(2*ow)));const auto* p=im.data+static_cast<ptrdiff_t>(sy)*im.stride+sx*3;auto* q=&out[(static_cast<size_t>(y)*ow+x)*3];q[0]=p[0];q[1]=p[1];q[2]=p[2];}
  }
}
static void resize_rgb_bilinear(const RgbView& im,int ow,int oh,std::vector<uint8_t>& out){
  for(int y=0;y<oh;++y){double fy=(y+.5)*im.crop.h/oh-.5;int y0=std::clamp(static_cast<int>(std::floor(fy)),0,im.crop.h-1),y1=std::min(y0+1,im.crop.h-1);double wy=std::clamp(fy-y0,0.0,1.0);y0+=im.crop.y;y1+=im.crop.y;
    for(int x=0;x<ow;++x){double fx=(x+.5)*im.crop.w/ow-.5;int x0=std::clamp(static_cast<int>(std::floor(fx)),0,im.crop.w-1),x1=std::min(x0+1,im.crop.w-1);double wx=std::clamp(fx-x0,0.0,1.0);x0+=im.crop.x;x1+=im.crop.x;const auto*a=im.data+static_cast<ptrdiff_t>(y0)*im.stride+x0*3;const auto*b=im.data+static_cast<ptrdiff_t>(y0)*im.stride+x1*3;const auto*c=im.data+static_cast<ptrdiff_t>(y1)*im.stride+x0*3;const auto*d=im.data+static_cast<ptrdiff_t>(y1)*im.stride+x1*3;auto*q=&out[(static_cast<size_t>(y)*ow+x)*3];for(int k=0;k<3;++k)q[k]=static_cast<uint8_t>(std::lround((1-wy)*((1-wx)*a[k]+wx*b[k])+wy*((1-wx)*c[k]+wx*d[k])));}
  }
}
static Rect source_crop(const Options&o,int width,int height,int native_w,int native_h){
  Rect r;
  if(o.crop_set)r=o.crop;
  else if(o.crop_mode==CropMode::full)r={0,0,width,height};
  else if(o.crop_mode==CropMode::center_native)r={(width-native_w)/2,(height-native_h)/2,native_w,native_h};
  else{int size=o.crop_size>0?o.crop_size:std::min(width,height);int x=o.crop_x>=0?o.crop_x:(width-size)/2;int y=o.crop_y>=0?o.crop_y:(height-size)/2;r={x,y,size,size};}
  if(r.w<=0||r.h<=0||r.x<0||r.y<0||r.x+r.w>width||r.y+r.h>height)fail("berekende RTSP-crop valt buiten het videoframe");
  return r;
}

static std::string av_error(int code){std::array<char,AV_ERROR_MAX_STRING_SIZE> text{};av_strerror(code,text.data(),text.size());return text.data();}
class RtspSource {
  const Options& o_; std::thread thread_; std::atomic_bool stop_{}; std::atomic<int64_t> deadline_ns_{};
  std::mutex mutex_; std::condition_variable cv_; std::vector<uint8_t> latest_,staging_;int width_{},height_{},stride_{};uint64_t sequence_{};
  int reported_width_{},reported_height_{};Rect reported_crop_{};
  SwsContext* sws_{};
  static int interrupt(void* opaque){auto*self=static_cast<RtspSource*>(opaque);return self->stop_||!running.load(std::memory_order_relaxed)||now_ns()>self->deadline_ns_.load();}
  static int64_t now_ns(){return std::chrono::duration_cast<Ns>(Clock::now().time_since_epoch()).count();}
  void deadline(){deadline_ns_=now_ns()+std::chrono::duration_cast<Ns>(std::chrono::seconds(5)).count();}
  void publish(AVFrame* frame){
    int w=frame->width,h=frame->height,stride=w*3;staging_.resize(static_cast<size_t>(stride)*h);
    sws_=sws_getCachedContext(sws_,w,h,static_cast<AVPixelFormat>(frame->format),w,h,AV_PIX_FMT_RGB24,SWS_FAST_BILINEAR,nullptr,nullptr,nullptr);
    if(!sws_)fail("libswscale-context maken mislukt");
    uint8_t* dst[]={staging_.data()};int lines[]={stride};
    if(sws_scale(sws_,frame->data,frame->linesize,0,h,dst,lines)!=h)fail("RTSP-frame naar RGB converteren mislukt");
    {std::lock_guard lock(mutex_);latest_.swap(staging_);width_=w;height_=h;stride_=stride;++sequence_;}cv_.notify_one();
  }
  void session(){
    AVFormatContext* format=avformat_alloc_context();if(!format)fail("AVFormatContext allocatie mislukt");format->interrupt_callback={interrupt,this};
    AVDictionary* options{};
    if(o_.url.starts_with("rtsp://")||o_.url.starts_with("rtsps://")){av_dict_set(&options,"rtsp_transport",o_.rtsp_transport==RtspTransport::tcp?"tcp":"udp",0);av_dict_set(&options,"fflags","nobuffer",0);av_dict_set(&options,"flags","low_delay",0);av_dict_set(&options,"max_delay","0",0);av_dict_set(&options,"stimeout","5000000",0);}
    deadline();int e=avformat_open_input(&format,o_.url.c_str(),nullptr,&options);av_dict_free(&options);if(e<0){if(format)avformat_close_input(&format);fail("RTSP openen: "+av_error(e));}
    deadline();e=avformat_find_stream_info(format,nullptr);if(e<0){avformat_close_input(&format);fail("RTSP streaminfo: "+av_error(e));}
    int stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);if(stream<0){avformat_close_input(&format);fail("geen RTSP-videostream gevonden");}
    const AVCodec* codec=avcodec_find_decoder(format->streams[stream]->codecpar->codec_id);if(!codec){avformat_close_input(&format);fail("geen decoder voor RTSP-codec");}
    AVCodecContext* context=avcodec_alloc_context3(codec);if(!context){avformat_close_input(&format);fail("AVCodecContext allocatie mislukt");}
    avcodec_parameters_to_context(context,format->streams[stream]->codecpar);context->flags|=AV_CODEC_FLAG_LOW_DELAY;context->thread_count=1;
    e=avcodec_open2(context,codec,nullptr);if(e<0){avcodec_free_context(&context);avformat_close_input(&format);fail("decoder openen: "+av_error(e));}
    AVPacket* packet=av_packet_alloc();AVFrame* frame=av_frame_alloc();if(!packet||!frame){av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&context);avformat_close_input(&format);fail("FFmpeg frameallocatie mislukt");}
    while(!stop_&&running.load(std::memory_order_relaxed)){
      deadline();e=av_read_frame(format,packet);if(e<0)break;
      if(packet->stream_index==stream&&avcodec_send_packet(context,packet)>=0){while(avcodec_receive_frame(context,frame)>=0){publish(frame);av_frame_unref(frame);}}
      av_packet_unref(packet);
    }
    av_packet_free(&packet);av_frame_free(&frame);avcodec_free_context(&context);avformat_close_input(&format);
    if(!stop_&&running.load(std::memory_order_relaxed))fail("RTSP-stream onderbroken: "+av_error(e));
  }
  void run(){
    while(!stop_&&running.load(std::memory_order_relaxed)){
      try{std::cerr<<"RTSP: verbinden via "<<(o_.rtsp_transport==RtspTransport::tcp?"tcp":"udp")<<" met "<<o_.url<<"\n";session();}
      catch(const std::exception&e){if(!stop_&&running.load())std::cerr<<"RTSP: "<<e.what()<<"; opnieuw proberen over 1s\n";}
      for(int i=0;i<10&&!stop_&&running.load();++i)std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
 public:
  explicit RtspSource(const Options&o):o_(o),thread_([this]{run();}){}
  ~RtspSource(){stop_=true;deadline_ns_=0;cv_.notify_all();if(thread_.joinable())thread_.join();if(sws_)sws_freeContext(sws_);}
  bool process_latest(uint64_t& seen,int ow,int oh,int native_w,int native_h,Filter filter,std::vector<uint8_t>& out){
    std::unique_lock lock(mutex_);cv_.wait_for(lock,std::chrono::milliseconds(200),[&]{return sequence_!=seen||stop_||!running.load();});if(sequence_==seen)return false;seen=sequence_;
    Rect crop=source_crop(o_,width_,height_,native_w,native_h);RgbView view{latest_.data(),width_,height_,stride_,crop};
    if(width_!=reported_width_||height_!=reported_height_||crop.x!=reported_crop_.x||crop.y!=reported_crop_.y||crop.w!=reported_crop_.w||crop.h!=reported_crop_.h){
      std::cerr<<"Capture: "<<width_<<"x"<<height_<<" (RTSP)\nCrop: "<<(o_.crop_set?"manual ":o_.crop_mode==CropMode::center_square?"center-square ":o_.crop_mode==CropMode::center_native?"center-native ":"full ")<<crop.w<<"x"<<crop.h<<"+"<<crop.x<<"+"<<crop.y<<"\n";
      reported_width_=width_;reported_height_=height_;reported_crop_=crop;
    }
    if(filter==Filter::nearest)resize_rgb_nearest(view,ow,oh,out);else resize_rgb_bilinear(view,ow,oh,out);return true;
  }
};
static void box_downsample(const std::vector<uint8_t>& in, int ow, int oh, int factor, std::vector<uint8_t>& out) {
  const int iw=ow*factor;
  const uint32_t divisor=static_cast<uint32_t>(factor*factor);
  for(int y=0;y<oh;++y) for(int x=0;x<ow;++x) {
    uint32_t sum[3]{};
    for(int dy=0;dy<factor;++dy) {
      const auto* p=&in[(static_cast<size_t>(y*factor+dy)*iw+x*factor)*3];
      for(int dx=0;dx<factor;++dx,p+=3){sum[0]+=p[0];sum[1]+=p[1];sum[2]+=p[2];}
    }
    auto* q=&out[(static_cast<size_t>(y)*ow+x)*3];
    for(int c=0;c<3;++c)q[c]=static_cast<uint8_t>((sum[c]+divisor/2)/divisor);
  }
}
static std::vector<float> gaussian_kernel(double sigma) {
  int radius=static_cast<int>(std::ceil(3.0*sigma));
  std::vector<float> kernel(static_cast<size_t>(2*radius+1)); double sum=0;
  for(int i=-radius;i<=radius;++i){double v=std::exp(-(i*i)/(2.0*sigma*sigma));kernel[static_cast<size_t>(i+radius)]=static_cast<float>(v);sum+=v;}
  for(float& v:kernel)v=static_cast<float>(v/sum);
  return kernel;
}
static void gaussian_blur(std::vector<uint8_t>& image, int w, int h, const std::vector<float>& kernel, std::vector<float>& scratch) {
  int radius=static_cast<int>(kernel.size()/2);
  for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<3;++c){
    float sum=0;for(int k=-radius;k<=radius;++k){int sx=std::clamp(x+k,0,w-1);sum+=kernel[static_cast<size_t>(k+radius)]*image[(static_cast<size_t>(y)*w+sx)*3+c];}
    scratch[(static_cast<size_t>(y)*w+x)*3+c]=sum;
  }
  for(int y=0;y<h;++y)for(int x=0;x<w;++x)for(int c=0;c<3;++c){
    float sum=0;for(int k=-radius;k<=radius;++k){int sy=std::clamp(y+k,0,h-1);sum+=kernel[static_cast<size_t>(k+radius)]*scratch[(static_cast<size_t>(sy)*w+x)*3+c];}
    image[(static_cast<size_t>(y)*w+x)*3+c]=static_cast<uint8_t>(std::clamp(std::lround(sum),0L,255L));
  }
}
class ColorCorrection {
  static constexpr int32_t one=4096;
  bool enabled_{};
  int32_t saturation_q_{},brightness_q_{};
  std::array<int32_t,256> contrast_{};
  std::array<uint8_t,256> gamma_{};
 public:
  explicit ColorCorrection(const Options& o):enabled_(o.color_correction),
    saturation_q_(static_cast<int32_t>(std::lround(o.saturation*one))),
    brightness_q_(static_cast<int32_t>(std::lround(o.brightness*one))) {
    for(int i=0;i<256;++i) {
      double contrasted=((static_cast<double>(i)-127.5)*o.contrast+127.5)*one;
      contrast_[static_cast<size_t>(i)]=static_cast<int32_t>(std::lround(contrasted));
      double corrected=o.gamma==1.0?i:std::pow(static_cast<double>(i)/255.0,1.0/o.gamma)*255.0;
      gamma_[static_cast<size_t>(i)]=static_cast<uint8_t>(std::clamp(std::lround(corrected),0L,255L));
    }
  }
  void apply(std::vector<uint8_t>& image) const {
    if(!enabled_)return;
    for(size_t i=0;i<image.size();i+=3) {
      int32_t r=contrast_[image[i]],g=contrast_[image[i+1]],b=contrast_[image[i+2]];
      // Rec.709 luminance weights in Q12: 0.2126, 0.7152, 0.0722.
      int32_t luminance=static_cast<int32_t>((static_cast<int64_t>(871)*r+static_cast<int64_t>(2929)*g+static_cast<int64_t>(296)*b)/one);
      r=luminance+static_cast<int32_t>((static_cast<int64_t>(r-luminance)*saturation_q_)/one);
      g=luminance+static_cast<int32_t>((static_cast<int64_t>(g-luminance)*saturation_q_)/one);
      b=luminance+static_cast<int32_t>((static_cast<int64_t>(b-luminance)*saturation_q_)/one);
      r=static_cast<int32_t>((static_cast<int64_t>(r)*brightness_q_)/one);
      g=static_cast<int32_t>((static_cast<int64_t>(g)*brightness_q_)/one);
      b=static_cast<int32_t>((static_cast<int64_t>(b)*brightness_q_)/one);
      auto finish=[&](int32_t value){int v=static_cast<int>((value+one/2)/one);return gamma_[static_cast<size_t>(std::clamp(v,0,255))];};
      image[i]=finish(r);image[i+1]=finish(g);image[i+2]=finish(b);
    }
  }
};
static void pattern(Pattern p, int w, int h, uint64_t frame, std::vector<uint8_t>& out) {
  for(int y=0;y<h;++y) for(int x=0;x<w;++x) { auto* q=&out[(static_cast<size_t>(y)*w+x)*3]; q[0]=q[1]=q[2]=0;
    if(p==Pattern::red)q[0]=255; else if(p==Pattern::green)q[1]=255; else if(p==Pattern::blue||p==Pattern::solid_blue)q[2]=255;
    else if(p==Pattern::checkerboard)q[0]=q[1]=q[2]=static_cast<uint8_t>((((x/8)+(y/8))&1)?255:0);
    else if(p==Pattern::line && x==static_cast<int>(frame%static_cast<uint64_t>(w)))q[0]=q[1]=q[2]=255;
  }
}

class DdpSender {
  // This exactly matches WLED's DDP_CHANNELS_PER_PACKET.
  static constexpr size_t max_payload=1440, header_size=10;
  int fd_=-1; sockaddr_storage addr_{}; socklen_t addr_len_{}; std::vector<std::array<uint8_t,header_size>> headers_;
  std::vector<iovec> iov_; std::vector<mmsghdr> msgs_; uint8_t sequence_=1; size_t bytes_{};
  bool debug_{}, debugged_{};
 public:
  DdpSender(const std::string& host, std::vector<uint8_t>& rgb, bool debug) : bytes_(rgb.size()), debug_(debug) {
    addrinfo hints{}; hints.ai_family=AF_UNSPEC; hints.ai_socktype=SOCK_DGRAM; hints.ai_protocol=IPPROTO_UDP; addrinfo* result{};
    int e=getaddrinfo(host.c_str(),"4048",&hints,&result); if(e) fail("DNS-resolutie mislukt voor "+host+": "+gai_strerror(e));
    fd_=socket(result->ai_family,SOCK_DGRAM|SOCK_CLOEXEC,IPPROTO_UDP); if(fd_<0){freeaddrinfo(result);fail("UDP-socket maken mislukt");}
    std::memcpy(&addr_,result->ai_addr,result->ai_addrlen); addr_len_=static_cast<socklen_t>(result->ai_addrlen); freeaddrinfo(result);
    size_t n=(bytes_+max_payload-1)/max_payload; headers_.resize(n); iov_.resize(n*2); msgs_.resize(n);
    for(size_t i=0;i<n;++i) { size_t off=i*max_payload,len=std::min(max_payload,bytes_-off); auto& h=headers_[i]; h.fill(0); h[0]=0x40|static_cast<uint8_t>(i+1==n?1:0); h[2]=1; h[3]=1; h[4]=static_cast<uint8_t>(off>>24);h[5]=static_cast<uint8_t>(off>>16);h[6]=static_cast<uint8_t>(off>>8);h[7]=static_cast<uint8_t>(off);h[8]=static_cast<uint8_t>(len>>8);h[9]=static_cast<uint8_t>(len);
      iov_[2*i]={h.data(),header_size}; iov_[2*i+1]={rgb.data()+off,len}; msgs_[i].msg_hdr.msg_name=&addr_;msgs_[i].msg_hdr.msg_namelen=addr_len_;msgs_[i].msg_hdr.msg_iov=&iov_[2*i];msgs_[i].msg_hdr.msg_iovlen=2;
    }
  }
  ~DdpSender(){if(fd_>=0)close(fd_);} DdpSender(const DdpSender&)=delete;
  void send() {
    // DDP uses a per-packet 4-bit sequence. Zero means "not used".
    for(size_t i=0;i<headers_.size();++i) {
      headers_[i][1]=sequence_;
      if(++sequence_ > 15) sequence_=1;
    }
    if(debug_ && !debugged_) {
      for(size_t i=0;i<headers_.size();++i) {
        const auto& h=headers_[i];
        uint32_t offset=(static_cast<uint32_t>(h[4])<<24)|(static_cast<uint32_t>(h[5])<<16)|(static_cast<uint32_t>(h[6])<<8)|h[7];
        uint16_t length=static_cast<uint16_t>((static_cast<uint16_t>(h[8])<<8)|h[9]);
        std::cerr<<"DDP packet "<<i<<": flags=0x"<<std::hex<<std::setw(2)<<std::setfill('0')<<static_cast<unsigned>(h[0])
          <<std::dec<<std::setfill(' ')<<" sequence="<<static_cast<unsigned>(h[1]&0x0f)
          <<" datatype="<<static_cast<unsigned>(h[2])<<" destination="<<static_cast<unsigned>(h[3])
          <<" offset="<<offset<<" length="<<length<<"\n";
      }
      debugged_=true;
    }
    int n=sendmmsg(fd_,msgs_.data(),static_cast<unsigned>(msgs_.size()),0);
    if(n!=static_cast<int>(msgs_.size())) fail("DDP sendmmsg mislukt of onvolledig");
  }
  size_t packets()const{return headers_.size();} size_t bytes()const{return bytes_;}
};

struct Stats { uint64_t frames{}, skipped{}; Ns capture{},resize{},color{},send{},total{}; };
static double ms(Ns n,uint64_t f){return f?std::chrono::duration<double,std::milli>(n).count()/static_cast<double>(f):0;}
static double cpu_seconds(){ timespec t{}; clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t); return static_cast<double>(t.tv_sec)+static_cast<double>(t.tv_nsec)/1e9; }
static void print_stats(const Stats&s,double wall,double cpu,const char* label){
  std::cerr<<std::fixed<<std::setprecision(2)<<label<<": fps="<<(wall>0?static_cast<double>(s.frames)/wall:0)<<" frames="<<s.frames<<" skipped="<<s.skipped
    <<" capture="<<ms(s.capture,s.frames)<<"ms resize="<<ms(s.resize,s.frames)<<"ms color="<<ms(s.color,s.frames)<<"ms send="<<ms(s.send,s.frames)<<"ms total="<<ms(s.total,s.frames)<<"ms cpu="<<(wall>0?100*cpu/wall:0)<<"%\n";
}
int main(int argc,char**argv){
 try {
  Options o=parse(argc,argv); std::signal(SIGINT,stop_handler); std::signal(SIGTERM,stop_handler);
  std::vector<uint8_t> rgb(static_cast<size_t>(o.width)*static_cast<size_t>(o.height)*3);
  int aa_factor=o.aa==AaMode::supersample?o.supersample:1;
  int work_w=o.width*aa_factor,work_h=o.height*aa_factor;
  std::vector<uint8_t> work;
  if(o.aa==AaMode::supersample)work.resize(static_cast<size_t>(work_w)*work_h*3);
  std::vector<float> kernel,scratch;
  if(o.aa==AaMode::gaussian){kernel=gaussian_kernel(o.aa_strength);scratch.resize(rgb.size());}
  ColorCorrection color(o);
  std::unique_ptr<XCapture> capture;
  std::unique_ptr<RtspSource> rtsp;
  if(o.pattern==Pattern::none&&o.source==Source::screen)capture=std::make_unique<XCapture>(o);
  std::cerr<<"Output: "<<o.width<<"x"<<o.height<<"\n";
  std::cerr<<"AA: "<<(o.aa==AaMode::off?"off":o.aa==AaMode::gaussian?"gaussian":"supersample");
  if(o.aa==AaMode::gaussian)std::cerr<<" strength="<<o.aa_strength;else if(o.aa==AaMode::supersample)std::cerr<<" factor="<<o.supersample;
  std::cerr<<"\n";
  if(o.color_correction)std::cerr<<std::fixed<<std::setprecision(2)<<"Color correction: saturation="<<o.saturation<<" contrast="<<o.contrast<<" brightness="<<o.brightness<<" gamma="<<o.gamma<<std::defaultfloat<<"\n";
  else std::cerr<<"Color correction: disabled\n";
  DdpSender sender(o.host,rgb,o.ddp_debug);
  std::cerr<<"DDP: "<<o.host<<":4048, "<<o.width<<"x"<<o.height<<", "<<sender.bytes()<<" bytes, "<<sender.packets()<<" pakketten/frame";
  std::cerr<<"\n";
  if(o.pattern==Pattern::none&&o.source==Source::rtsp)rtsp=std::make_unique<RtspSource>(o);
  Stats all{},interval{}; auto start=Clock::now(), report=start, deadline=start; double cpu0=cpu_seconds(),report_cpu=cpu0; uint64_t frame=0,rtsp_sequence=0;
  while(running.load(std::memory_order_relaxed)) {
    auto fs=Clock::now(); if(o.fps>0) {
      auto period=Ns(static_cast<int64_t>(1e9/o.fps));
      if(fs<deadline) { std::this_thread::sleep_until(deadline); fs=Clock::now(); }
      if(fs>deadline+period) { auto n=static_cast<uint64_t>((fs-deadline)/period); all.skipped+=n;interval.skipped+=n;deadline+=period*static_cast<int64_t>(n); }
      deadline+=period;
    }
    auto a=Clock::now(); if(capture)capture->grab();
    auto& target=o.aa==AaMode::supersample?work:rgb;
    if(rtsp&&!rtsp->process_latest(rtsp_sequence,work_w,work_h,work_w,work_h,o.filter,target))continue;
    auto b=Clock::now();
    if(capture) {
      if(o.filter==Filter::nearest)resize_nearest(capture->image(),work_w,work_h,target);else resize_bilinear(capture->image(),work_w,work_h,target);
    }
    if(capture||rtsp){
      if(o.aa==AaMode::supersample)box_downsample(work,o.width,o.height,o.supersample,rgb);
      else if(o.aa==AaMode::gaussian)gaussian_blur(rgb,o.width,o.height,kernel,scratch);
    }
    else pattern(o.pattern,o.width,o.height,frame,rgb);
    auto c0=Clock::now(); if(capture||rtsp)color.apply(rgb); auto c=Clock::now(); sender.send(); auto d=Clock::now();
    Ns ca=b-a,re=c0-b,co=c-c0,se=d-c,to=d-fs; ++all.frames;++interval.frames;++frame; all.capture+=ca;interval.capture+=ca;all.resize+=re;interval.resize+=re;all.color+=co;interval.color+=co;all.send+=se;interval.send+=se;all.total+=to;interval.total+=to;
    if(o.benchmark&&d-report>=std::chrono::seconds(1)){double wall=std::chrono::duration<double>(d-report).count(),cpu=cpu_seconds();print_stats(interval,wall,cpu-report_cpu,"interval");interval={};report=d;report_cpu=cpu;}
  }
  auto end=Clock::now(); print_stats(all,std::chrono::duration<double>(end-start).count(),cpu_seconds()-cpu0,"totaal");
 } catch(const std::exception&e){std::cerr<<"Fout: "<<e.what()<<"\n";return 1;} return 0;
}
