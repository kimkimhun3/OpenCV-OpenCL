// clahe_timed.cpp
// Build:
//   g++ -O3 -DNDEBUG -std=c++17 clahe_timed.cpp -o clahe \
//       $(pkg-config --cflags --libs opencv4)
//
// Run example:
//   ./clahe --input=2K.jpg --clipLimit=2 --tileGridSize=8
// Output:
//   2K2-8x8.jpg
//   Prints the time (ms) spent inside CLAHE only.

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <chrono>
#include <iomanip>

static bool parse_kv(const char* arg, const char* key, std::string& val) {
    std::string prefix = std::string("--") + key + "=";
    if (std::strncmp(arg, prefix.c_str(), prefix.size()) == 0) {
        val.assign(arg + prefix.size());
        return true;
    }
    return false;
}

static std::string basename_no_ext(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string file = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    return (dot == std::string::npos) ? file : file.substr(0, dot);
}

static std::string extension_with_dot(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string file = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = file.find_last_of('.');
    return (dot == std::string::npos) ? std::string(".jpg") : file.substr(dot);
}

static std::string clip_to_string_for_filename(double clip) {
    long long iv = static_cast<long long>(std::llround(clip));
    if (std::abs(clip - static_cast<double>(iv)) < 1e-9) return std::to_string(iv);
    std::string s = std::to_string(clip);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    for (auto& c : s) if (c == '.') c = 'p';
    return s;
}

int main(int argc, char** argv) {
    std::string inputPath;
    double clipLimit = 3.0;
    int tile = 4;

    for (int i = 1; i < argc; ++i) {
        std::string v;
        if (parse_kv(argv[i], "input", v))            inputPath = v;
        else if (parse_kv(argv[i], "clipLimit", v))   { try { clipLimit = std::stod(v); } catch (...) {} }
        else if (parse_kv(argv[i], "tileGridSize", v)){ try { tile = std::stoi(v); } catch (...) {} }
        else if (parse_kv(argv[i], "tile", v))        { try { tile = std::stoi(v); } catch (...) {} } // alias
        else std::cerr << "Warning: ignoring unknown arg: " << argv[i] << "\n";
    }

    if (inputPath.empty()) {
        std::cerr << "Usage: " << (argc ? argv[0] : "clahe")
                  << " --input=<image> [--clipLimit=3.0] [--tileGridSize=4]\n";
        return 1;
    }
    if (clipLimit <= 0.0) { std::cerr << "Error: --clipLimit must be > 0\n"; return 1; }
    if (tile < 1)         { std::cerr << "Error: --tileGridSize must be >= 1\n"; return 1; }

    cv::Mat bgr = cv::imread(inputPath, cv::IMREAD_COLOR);
    if (bgr.empty()) {
        std::cerr << "Error: cannot open image: " << inputPath << "\n";
        return 1;
    }

    // BGR -> YUV, split planes (not timed)
    cv::Mat yuv;
    cv::cvtColor(bgr, yuv, cv::COLOR_BGR2YUV);
    std::vector<cv::Mat> planes(3);
    cv::split(yuv, planes); // planes[0] = Y

    // Prepare CLAHE and output buffer
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit, cv::Size(tile, tile));
    cv::Mat Y_eq(planes[0].size(), planes[0].type()); // pre-allocate so timing is only the compute

    // === TIMED SECTION: CLAHE compute only ===
    auto t0 = std::chrono::steady_clock::now();
    clahe->apply(planes[0], Y_eq);
    auto t1 = std::chrono::steady_clock::now();
    double clahe_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    // =========================================

    // Recombine and convert back to BGR (not timed)
    Y_eq.copyTo(planes[0]);
    cv::merge(planes, yuv);
    cv::Mat out_bgr;
    cv::cvtColor(yuv, out_bgr, cv::COLOR_YUV2BGR);

    // Save single output with requested naming
    const std::string base = basename_no_ext(inputPath);
    const std::string ext  = extension_with_dot(inputPath);
    const std::string clipStr = clip_to_string_for_filename(clipLimit);
    const std::string outName = base + clipStr + "-" + std::to_string(tile) + "x" + std::to_string(tile) + ext;

    if (!cv::imwrite(outName, out_bgr)) {
        std::cerr << "Error: failed to write output: " << outName << "\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "CLAHE_apply_time_ms=" << clahe_ms
              << " (clipLimit=" << clipLimit
              << ", tileGridSize=" << tile << "x" << tile << ")\n";
    std::cout << "Saved: " << outName << "\n";
    return 0;
}


/////////////////////////////////
/*
 * OpenCV CLAHE on NV12 Y-channel with dynamic parameters
 */
// #include <opencv2/opencv.hpp>
// #include <iostream>
// #include <chrono>
// #include <string>
// #include <filesystem>

// void printUsage(const char* programName) {
//     std::cout << "Usage: " << programName << " --input=<image_path> --clipLimit=<value> --tileGridSize=<size>" << std::endl;
//     std::cout << "Example: " << programName << " --input=2K.jpg --clipLimit=2.0 --tileGridSize=8" << std::endl;
//     std::cout << "  clipLimit: Threshold for contrast limiting (e.g., 2.0)" << std::endl;
//     std::cout << "  tileGridSize: Grid size for tiles (e.g., 8 for 8x8 grid)" << std::endl;
// }

// std::string extractFilename(const std::string& path) {
//     // Find last slash or backslash
//     size_t lastSlash = path.find_last_of("/\\");
//     std::string filename = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
    
//     // Remove extension
//     size_t lastDot = filename.find_last_of('.');
//     return (lastDot == std::string::npos) ? filename : filename.substr(0, lastDot);
// }

// std::string generateOutputFilename(const std::string& inputPath, double clipLimit, int tileSize) {
//     std::string baseName = extractFilename(inputPath);
    
//     // Format clipLimit to remove unnecessary decimal places
//     std::string clipStr;
//     if (clipLimit == (int)clipLimit) {
//         clipStr = std::to_string((int)clipLimit);
//     } else {
//         clipStr = std::to_string(clipLimit);
//         // Remove trailing zeros
//         clipStr.erase(clipStr.find_last_not_of('0') + 1, std::string::npos);
//         clipStr.erase(clipStr.find_last_not_of('.') + 1, std::string::npos);
//     }
    
//     return baseName + clipStr + "-" + std::to_string(tileSize) + "x" + std::to_string(tileSize) + ".jpg";
// }

// int main(int argc, char** argv) {
//     if (argc != 4) {
//         printUsage(argv[0]);
//         return -1;
//     }

//     std::string inputPath;
//     double clipLimit = 2.0;
//     int tileGridSize = 8;

//     // Parse command line arguments
//     for (int i = 1; i < argc; i++) {
//         std::string arg(argv[i]);
        
//         if (arg.find("--input=") == 0) {
//             inputPath = arg.substr(8);
//         } else if (arg.find("--clipLimit=") == 0) {
//             clipLimit = std::stod(arg.substr(12));
//         } else if (arg.find("--tileGridSize=") == 0) {
//             tileGridSize = std::stoi(arg.substr(15));
//         } else {
//             std::cerr << "Unknown argument: " << arg << std::endl;
//             printUsage(argv[0]);
//             return -1;
//         }
//     }

//     if (inputPath.empty()) {
//         std::cerr << "Input path is required!" << std::endl;
//         printUsage(argv[0]);
//         return -1;
//     }

//     // Validate parameters
//     if (clipLimit <= 0) {
//         std::cerr << "clipLimit must be greater than 0!" << std::endl;
//         return -1;
//     }
    
//     if (tileGridSize <= 0 || tileGridSize > 64) {
//         std::cerr << "tileGridSize must be between 1 and 64!" << std::endl;
//         return -1;
//     }

//     // -------------------- Load Image --------------------
//     cv::Mat bgr = cv::imread(inputPath);
//     if (bgr.empty()) {
//         fprintf(stderr, "Cannot open image: %s\n", inputPath.c_str());
//         return -1;
//     }

//     int height = bgr.rows;
//     int width = bgr.cols;
//     std::cout << "Input image: " << inputPath << std::endl;
//     std::cout << "Image dimensions: " << width << "x" << height << std::endl;
//     std::cout << "CLAHE parameters: clipLimit=" << clipLimit << ", tileGridSize=" << tileGridSize << "x" << tileGridSize << std::endl;

//     // Convert BGR to YUV I420 (YUV420 planar)
//     cv::Mat yuv;
//     cv::cvtColor(bgr, yuv, cv::COLOR_BGR2YUV_I420);

//     // Extract Y plane (first height*width bytes)
//     cv::Mat y_plane(height, width, CV_8UC1, yuv.data);

//     // Output buffer
//     cv::Mat y_clahe(height, width, CV_8UC1);

//     // -------------------- CLAHE Processing --------------------
//     cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE();
//     clahe->setClipLimit(clipLimit);
//     clahe->setTilesGridSize(cv::Size(tileGridSize, tileGridSize));

//     auto t1 = std::chrono::high_resolution_clock::now();
//     clahe->apply(y_plane, y_clahe);
//     auto t2 = std::chrono::high_resolution_clock::now();
    
//     double clahe_time = std::chrono::duration<double, std::milli>(t2 - t1).count();
//     std::cout << "CLAHE processing time: " << clahe_time << " ms" << std::endl;

//     // -------------------- Save Output Image --------------------
//     std::string outputFilename = generateOutputFilename(inputPath, clipLimit, tileGridSize);
//     cv::imwrite(outputFilename, y_clahe);
    
//     std::cout << "Output saved as: " << outputFilename << std::endl;
//     std::cout << "Processing completed successfully!" << std::endl;
    
//     return 0;
// }