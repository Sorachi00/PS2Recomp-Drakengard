function(ps2x_add_gpu_backend target)
    get_filename_component(runtime "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/gs_shaders")
    if(NOT TARGET ps2x_gs_shaders)
        find_program(PS2X_FXC fxc
            HINTS "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
                  "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
            REQUIRED)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${runtime}/src/lib/gs/gs_gpu.hlsl"
            "${runtime}/src/lib/gs/gs_gpu_tables.hlsl")
        file(READ "${runtime}/src/lib/gs/gs_gpu_tables.hlsl" tables)
        file(READ "${runtime}/src/lib/gs/gs_gpu.hlsl" shader)
        file(GENERATE OUTPUT "${shader_dir}/gs_gpu.hlsl" CONTENT "${tables}\n${shader}")
        foreach(entry Raster Transfer Convert Compose)
            add_custom_command(OUTPUT "${shader_dir}/${entry}.h"
                COMMAND "${PS2X_FXC}" /nologo /T cs_5_0 /E ${entry} /Gis /O3
                    /Vn kGs${entry} /Fh "${shader_dir}/${entry}.h" "${shader_dir}/gs_gpu.hlsl"
                DEPENDS "${runtime}/src/lib/gs/gs_gpu.hlsl" "${runtime}/src/lib/gs/gs_gpu_tables.hlsl"
                COMMENT "Compiling GS GPU ${entry}" VERBATIM)
            list(APPEND headers "${shader_dir}/${entry}.h")
        endforeach()
        add_custom_target(ps2x_gs_shaders DEPENDS ${headers})
    endif()
    add_dependencies(${target} ps2x_gs_shaders)
    target_sources(${target} PRIVATE "${runtime}/src/lib/gs/gs_gpu_backend.cpp")
    target_include_directories(${target} PRIVATE "${shader_dir}")
    target_link_libraries(${target} PUBLIC d3d11 dxgi)
endfunction()
