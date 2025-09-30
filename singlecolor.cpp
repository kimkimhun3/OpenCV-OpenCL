// single_frame_test.cpp
// Test histogram equalization on a single JPG image
// Converts JPG -> YUV -> Apply equalizeHist on Y channel -> Save result
//
// Build:
// g++ -O3 -DNDEBUG -std=c++17 single_frame_test.cpp -o single_frame_test \
//   $(pkg-config --cflags --libs opencv4)

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <string>
#include <chrono>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <input.jpg> <output.jpg>" << std::endl;
        std::cout << "Example: " << argv[0] << " input.jpg output_enhanced.jpg" << std::endl;
        return -1;
    }

    std::string input_file = argv[1];
    std::string output_file = argv[2];

    auto start_time = std::chrono::high_resolution_clock::now();

    // Step 1: Load the input JPG image
    cv::Mat bgr_image = cv::imread(input_file, cv::IMREAD_COLOR);
    if (bgr_image.empty()) {
        std::cerr << "Error: Could not load image " << input_file << std::endl;
        return -1;
    }

    std::cout << "Loaded image: " << bgr_image.cols << "x" << bgr_image.rows << std::endl;
    auto load_time = std::chrono::high_resolution_clock::now();

    // Step 2: Convert BGR to YUV (YUV420 format like NV12 but with separate U,V planes)
    cv::Mat yuv_image;
    cv::cvtColor(bgr_image, yuv_image, cv::COLOR_BGR2YUV);
    auto convert_to_yuv_time = std::chrono::high_resolution_clock::now();

    // Step 3: Split YUV channels
    std::vector<cv::Mat> yuv_channels;
    cv::split(yuv_image, yuv_channels);
    
    cv::Mat y_channel = yuv_channels[0];  // Luminance (brightness)
    cv::Mat u_channel = yuv_channels[1];  // Chrominance U (blue-yellow)
    cv::Mat v_channel = yuv_channels[2];  // Chrominance V (red-cyan)
    
    std::cout << "Y channel size: " << y_channel.cols << "x" << y_channel.rows << std::endl;
    auto split_time = std::chrono::high_resolution_clock::now();

    // Step 4: Apply histogram equalization ONLY to Y channel
    cv::Mat y_equalized;
    cv::equalizeHist(y_channel, y_equalized);
    auto equalize_time = std::chrono::high_resolution_clock::now();

    // Step 5: Merge channels back (enhanced Y + original U,V)
    std::vector<cv::Mat> enhanced_yuv_channels = {y_equalized, u_channel, v_channel};
    cv::Mat enhanced_yuv;
    cv::merge(enhanced_yuv_channels, enhanced_yuv);
    auto merge_time = std::chrono::high_resolution_clock::now();

    // Step 6: Convert back to BGR for saving as JPG
    cv::Mat enhanced_bgr;
    cv::cvtColor(enhanced_yuv, enhanced_bgr, cv::COLOR_YUV2BGR);
    auto convert_to_bgr_time = std::chrono::high_resolution_clock::now();

    // Step 7: Save the result
    bool save_success = cv::imwrite(output_file, enhanced_bgr);
    auto save_time = std::chrono::high_resolution_clock::now();

    if (save_success) {
        std::cout << "Enhanced image saved to: " << output_file << std::endl;
    } else {
        std::cerr << "Error: Could not save image to " << output_file << std::endl;
        return -1;
    }

    // Performance timing
    auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(save_time - start_time);
    auto load_us = std::chrono::duration_cast<std::chrono::microseconds>(load_time - start_time);
    auto yuv_us = std::chrono::duration_cast<std::chrono::microseconds>(convert_to_yuv_time - load_time);
    auto split_us = std::chrono::duration_cast<std::chrono::microseconds>(split_time - convert_to_yuv_time);
    auto eq_us = std::chrono::duration_cast<std::chrono::microseconds>(equalize_time - split_time);
    auto merge_us = std::chrono::duration_cast<std::chrono::microseconds>(merge_time - equalize_time);
    auto bgr_us = std::chrono::duration_cast<std::chrono::microseconds>(convert_to_bgr_time - merge_time);
    auto save_us = std::chrono::duration_cast<std::chrono::microseconds>(save_time - convert_to_bgr_time);

    std::cout << "\n=== PERFORMANCE BREAKDOWN ===" << std::endl;
    std::cout << "Image loading:        " << load_us.count() << " μs" << std::endl;
    std::cout << "BGR->YUV conversion:  " << yuv_us.count() << " μs" << std::endl;
    std::cout << "Channel splitting:    " << split_us.count() << " μs" << std::endl;
    std::cout << "Histogram equalize:   " << eq_us.count() << " μs" << std::endl;
    std::cout << "Channel merging:      " << merge_us.count() << " μs" << std::endl;
    std::cout << "YUV->BGR conversion:  " << bgr_us.count() << " μs" << std::endl;
    std::cout << "Image saving:         " << save_us.count() << " μs" << std::endl;
    std::cout << "TOTAL PROCESSING:     " << total_time.count() << " μs (" 
              << total_time.count() / 1000.0 << " ms)" << std::endl;

    // Calculate potential FPS based on processing time only (excluding I/O)
    auto processing_time = eq_us + merge_us + yuv_us + split_us + bgr_us;
    if (processing_time.count() > 0) {
        double potential_fps = 1000000.0 / processing_time.count();
        std::cout << "Core processing time: " << processing_time.count() << " μs" << std::endl;
        std::cout << "Potential FPS:        " << potential_fps << " fps" << std::endl;
    }

    return 0;
}