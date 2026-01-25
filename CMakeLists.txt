cmake_minimum_required(VERSION 3.16)
project(ShieldOrderRithmic VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Rithmic API path - set this to your R|API+ installation
set(RITHMIC_API_DIR "${CMAKE_SOURCE_DIR}/rithmic_api" CACHE PATH "Path to Rithmic R|API+ SDK")

# Source files
set(SOURCES
    src/main.cpp
    src/rithmic_connection.cpp
    src/market_data_handler.cpp
    src/strategy_engine.cpp
    src/order_manager.cpp
    src/risk_manager.cpp
    src/position_scaler.cpp
    src/config.cpp
    src/logger.cpp
    # Order flow analysis modules
    src/tape_reader.cpp
    src/footprint_engine.cpp
    src/book_analyzer.cpp
    src/market_structure.cpp
    src/scalp_engine.cpp
)

add_executable(shieldorder ${SOURCES})

target_include_directories(shieldorder PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${RITHMIC_API_DIR}/include
)

# Link Rithmic API library when available
if(EXISTS "${RITHMIC_API_DIR}/lib")
    target_link_directories(shieldorder PRIVATE ${RITHMIC_API_DIR}/lib)
    target_link_libraries(shieldorder PRIVATE RApi)
endif()

target_link_libraries(shieldorder PRIVATE pthread)

# Optional: JSON config parsing (nlohmann/json header-only)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/json.hpp")
    target_compile_definitions(shieldorder PRIVATE HAS_JSON)
endif()
