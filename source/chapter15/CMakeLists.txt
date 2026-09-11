
raptor_add_chapter(
	Chapter15
	main.cpp
)
raptor_link_chapter_deps(Chapter15)

target_compile_definitions(Chapter15 PRIVATE
  RAPTOR_USE_COOPVEC
)

target_include_directories(Chapter15 PRIVATE
	../shaders/slang
)

# This is left as reference. The MaterialX library is very large and won't be distributed with the engine

# add_executable(matx_material_builder
# 	matx_material_builder.cpp
# )

# set_property(TARGET matx_material_builder PROPERTY CXX_STANDARD 20)

# target_compile_options(matx_material_builder PRIVATE
# 	/MD
# )

# target_link_directories(matx_material_builder PRIVATE
#   MaterialX/_out/lib
# )

# target_link_libraries(matx_material_builder
#   MaterialXCore
#   MaterialXFormat
#   MaterialXGenHw
#   MaterialXGenShader
#   MaterialXGenSlang
# )

# target_include_directories(matx_material_builder PRIVATE
#   _out/include
# )
