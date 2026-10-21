cmake_minimum_required(VERSION 3.16)

# Gom cả gói protocol nội bộ LẪN thư mục components local vào danh sách bắt buộc quét
set(EXTRA_COMPONENT_DIRS "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common"
                         "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Temp_Control)