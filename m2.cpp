// nv12_style_test.cpp
// Test that mimics the NV12 processing workflow from your streaming code
// Converts JPG -> YUV -> Process Y channel directly -> Reconstruct -> Save
//
// Build:
// g++ -O3 -DNDEBUG -std=c++17 nv12_style_test.cpp -o nv12_style_test \
//   $(pkg-config --cflags --libs opencv4)

#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <string>
#include <chrono>

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "Usage: " << argv[0] << " <input.jpg> <output.jpg>" << std::endl;
        return -1;
    }

    std::string input_file = argv[1];
    std::string output_file = argv[2];

    auto start_time = std::chrono::high_resolution_clock::now();

    // Load input image
    cv::Mat bgr_image = cv::imread(input_file, cv::IMREAD_COLOR);
    if (bgr_image.empty()) {
        std::cerr << "Error: Could not load image " << input_file << std::endl;
        return -1;
    }

    int width = bgr_image.cols;
    int height = bgr_image.rows;
    std::cout << "Processing image: " << width << "x" << height << std::endl;

    // Convert to YUV for processing (similar to your NV12 workflow)
    cv::Mat yuv_full;
    cv::cvtColor(bgr_image, yuv_full, cv::COLOR_BGR2YUV);
    auto convert_time = std::chrono::high_resolution_clock::now();

    // METHOD 1: Direct Y plane processing (like your optimized version)
    std::cout << "\n=== METHOD 1: Direct Y-plane processing ===" << std::endl;
    auto method1_start = std::chrono::high_resolution_clock::now();
    
    // Create output image with same size
    cv::Mat enhanced_yuv_direct = yuv_full.clone();
    
    // Create Mat view of just the Y channel (first channel)
    std::vector<cv::Mat> yuv_channels_direct;
    cv::split(yuv_full, yuv_channels_direct);
    
    // Apply histogram equalization directly to Y channel
    cv::Mat y_enhanced_direct;
    cv::equalizeHist(yuv_channels_direct[0], y_enhanced_direct);
    
    // Replace Y channel in output (keep U,V unchanged)
    std::vector<cv::Mat> output_channels = {y_enhanced_direct, yuv_channels_direct[1], yuv_channels_direct[2]};
    cv::Mat enhanced_yuv_method1;
    cv::merge(output_channels, enhanced_yuv_method1);
    
    auto method1_end = std::chrono::high_resolution_clock::now();
    auto method1_time = std::chrono::duration_cast<std::chrono::microseconds>(method1_end - method1_start);
    
    // METHOD 2: Zero-copy processing (most similar to your streaming code)
    std::cout << "\n=== METHOD 2: Zero-copy processing ===" << std::endl;
    auto method2_start = std::chrono::high_resolution_clock::now();
    
    // Simulate the streaming approach - work directly on buffer data
    cv::Mat yuv_buffer = yuv_full.clone();
    
    // Extract Y plane without copy (create Mat view)
    cv::Mat y_plane_view(height, width, CV_8UC1, yuv_buffer.data);
    
    // Create output buffer
    cv::Mat output_buffer = yuv_buffer.clone();
    cv::Mat y_output_view(height, width, CV_8UC1, output_buffer.data);
    
    // Apply histogram equalization directly between buffer views
    cv::equalizeHist(y_plane_view, y_output_view);
    
    // UV channels are automatically preserved since we only modified Y plane
    
    auto method2_end = std::chrono::high_resolution_clock::now();
    auto method2_time = std::chrono::duration_cast<std::chrono::microseconds>(method2_end - method2_start);
    
    // Convert result back to BGR and save
    cv::Mat result_bgr;
    cv::cvtColor(enhanced_yuv_method1, result_bgr, cv::COLOR_YUV2BGR);
    
    auto save_start = std::chrono::high_resolution_clock::now();
    bool save_success = cv::imwrite(output_file, result_bgr);
    auto save_end = std::chrono::high_resolution_clock::now();
    
    if (save_success) {
        std::cout << "\nEnhanced image saved to: " << output_file << std::endl;
    } else {
        std::cerr << "Error: Could not save image" << std::endl;
        return -1;
    }

    // Performance analysis
    auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(save_end - start_time);
    auto convert_us = std::chrono::duration_cast<std::chrono::microseconds>(convert_time - start_time);
    auto save_us = std::chrono::duration_cast<std::chrono::microseconds>(save_end - save_start);

    std::cout << "\n=== PERFORMANCE COMPARISON ===" << std::endl;
    std::cout << "Image loading + BGR->YUV:  " << convert_us.count() << " μs" << std::endl;
    std::cout << "Method 1 (split/merge):    " << method1_time.count() << " μs" << std::endl;
    std::cout << "Method 2 (zero-copy):      " << method2_time.count() << " μs" << std::endl;
    std::cout << "Saving result:             " << save_us.count() << " μs" << std::endl;
    std::cout << "Total time:                " << total_time.count() << " μs" << std::endl;
    
    // Calculate FPS potential for core processing
    double fps_method1 = method1_time.count() > 0 ? 1000000.0 / method1_time.count() : 0;
    double fps_method2 = method2_time.count() > 0 ? 1000000.0 / method2_time.count() : 0;
    
    std::cout << "\nPotential FPS (processing only):" << std::endl;
    std::cout << "Method 1: " << fps_method1 << " fps" << std::endl;
    std::cout << "Method 2: " << fps_method2 << " fps" << std::endl;
    
    // Performance improvement
    if (method1_time.count() > 0 && method2_time.count() > 0) {
        double speedup = (double)method1_time.count() / method2_time.count();
        std::cout << "Method 2 speedup: " << speedup << "x faster" << std::endl;
    }

    return 0;
}