function(add_dswp_pass NAME)
  add_library(${NAME} MODULE ${ARGN})
  if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    target_link_options(${NAME} BEFORE PRIVATE -undefined dynamic_lookup)
  endif()
  set_target_properties(${NAME} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    PREFIX ""
  )
endfunction()

add_dswp_pass(DSWPAnalyzer  DSWPAnalyzer.cpp PDG.cpp Partition.cpp)
add_dswp_pass(DSWPTransform DSWPTransform.cpp PDG.cpp Partition.cpp)
