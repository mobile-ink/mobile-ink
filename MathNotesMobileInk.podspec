Pod::Spec.new do |s|
  s.name         = "MathNotesMobileInk"
  s.version      = "0.2.0"
  s.summary      = "Native Skia/Metal mobile ink engine for React Native"
  s.homepage     = "https://github.com/mathnotes-app/mobile-ink"
  s.license      = { :type => "Apache-2.0", :file => "LICENSE" }
  s.authors      = { "BuilderPro LLC" => "mark@builderproapps.com" }
  s.platforms    = { :ios => "15.1" }
  s.source       = { :git => "https://github.com/mathnotes-app/mobile-ink.git", :tag => "#{s.version}" }

  s.source_files = "ios/MobileInkModule/**/*.{h,m,mm,swift}", "cpp/**/*.{h,cpp}"
  s.private_header_files = "cpp/*.h"
  s.requires_arc = true
  s.swift_version = "5.0"

  s.dependency "react-native-skia"
  s.dependency "React-Core"

  s.pod_target_xcconfig = {
    "HEADER_SEARCH_PATHS" => "\"$(PODS_TARGET_SRCROOT)\" \"$(PODS_TARGET_SRCROOT)/cpp\" \"$(PODS_ROOT)/../../node_modules/@shopify/react-native-skia/cpp/\"/**",
    "CLANG_CXX_LANGUAGE_STANDARD" => "c++17",
    "CLANG_CXX_LIBRARY" => "libc++",
    "OTHER_LDFLAGS" => "$(inherited) -lc++",
    "GCC_PREPROCESSOR_DEFINITIONS" => "$(inherited) SK_METAL=1",
    "DEFINES_MODULE" => "YES"
  }

  s.frameworks = "MetalKit", "Metal", "PDFKit", "CoreGraphics", "UIKit"
  s.libraries = "c++"
end
