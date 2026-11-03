# Playable client (architecture §3, §11 M2). Placeholder until the platform
# work unit lands SDL3 + sokol_gfx + Dear ImGui.
add_executable(kfm_client main.cpp)
target_link_libraries(kfm_client PRIVATE kfm::sim kfm::presentation kfm::platform_input kfm::warnings)
set_target_properties(kfm_client PROPERTIES FOLDER apps)
