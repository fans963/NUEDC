#pragma once

#include "core/component.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <atomic>
#include <mutex>
#include <thread>

namespace nuedcs::vision {

/// Vision test component — camera capture + Canny edge detection.
///
/// Capture runs on a background thread to avoid blocking the main loop.
///
/// Outputs:
///   /vision/frame_width   — captured frame width (int)
///   /vision/frame_height  — captured frame height (int)
///   /vision/edge_count    — number of edge pixels (int)
///
/// Config:
///   device       — camera device path or index, default 0
///   width        — capture width, default 640
///   height       — capture height, default 480
///   canny_low    — Canny low threshold, default 50
///   canny_high   — Canny high threshold, default 150
///   blur_ksize   — Gaussian blur kernel size (odd), default 5
///   save_path    — save edge image to this path (empty = disabled)
class VisionTest final : public core::Component {
public:
    explicit VisionTest(ryml::NodeRef config) {
        auto c = core::Config{config};

        device_     = c["device"].get(0);
        width_      = c["width"].get(640);
        height_     = c["height"].get(480);
        canny_low_  = c["canny_low"].get(50);
        canny_high_ = c["canny_high"].get(150);
        blur_ksize_ = c["blur_ksize"].get(5);
        save_path_  = c["save_path"].str("");

        register_output("/vision/frame_width", out_width_, 0);
        register_output("/vision/frame_height", out_height_, 0);
        register_output("/vision/edge_count", out_edges_, 0);
        register_output("/vision/image", out_image_);
    }

    ~VisionTest() override {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) thread_.join();
        cap_.release();
    }

    bool init() override {
        cap_.open(device_);
        if (!cap_.isOpened()) {
            error("Failed to open camera device {}", device_);
            return false;
        }
        cap_.set(cv::CAP_PROP_FRAME_WIDTH, width_);
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, height_);

        info("Camera opened: {}x{} on device {}", width_, height_, device_);

        // 启动后台采集线程
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread(&VisionTest::capture_loop, this);
        return true;
    }

    void update() override {
        // 非阻塞：读取后台线程的最新结果
        std::lock_guard lk(mtx_);
        *out_width_  = latest_width_;
        *out_height_ = latest_height_;
        *out_edges_  = latest_edges_;
        *out_image_  = latest_image_.clone();  // 深拷贝给 bridge
    }

private:
    void capture_loop() {
        static int frame_ok = 0, frame_fail = 0;
        while (running_.load(std::memory_order_relaxed)) {
            cv::Mat frame;
            if (!cap_.read(frame) || frame.empty()) {
                if (++frame_fail == 1) warn("Failed to read frame (first)");
                continue;
            }
            if (++frame_ok == 1)
                info("First frame captured: {}x{} ch={}", frame.cols, frame.rows, frame.channels());

            cv::Mat gray, blurred, edges;
            cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
            cv::GaussianBlur(gray, blurred, cv::Size(blur_ksize_, blur_ksize_), 0);
            cv::Canny(blurred, edges, canny_low_, canny_high_);

            int w = frame.cols, h = frame.rows, e = cv::countNonZero(edges);

            if (!save_path_.empty())
                cv::imwrite(save_path_, edges);

            {
                std::lock_guard lk(mtx_);
                latest_width_  = w;
                latest_height_ = h;
                latest_edges_  = e;
                latest_image_  = frame.clone();  // 深拷贝
            }
        }
    }

    int device_     = 0;
    int width_      = 640;
    int height_     = 480;
    int canny_low_  = 50;
    int canny_high_ = 150;
    int blur_ksize_ = 5;
    std::string save_path_;

    cv::VideoCapture cap_;

    // 后台线程
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mtx_;
    int latest_width_  = 0;
    int latest_height_ = 0;
    int latest_edges_  = 0;
    cv::Mat latest_image_;

    OutputInterface<int> out_width_;
    OutputInterface<int> out_height_;
    OutputInterface<int> out_edges_;
    OutputInterface<cv::Mat> out_image_;
};

} // namespace nuedcs::vision
