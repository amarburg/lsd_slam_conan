# Set source files
set(lsdgui_SOURCE_FILES
  GUI.cpp
  LSDArgs.cpp
  Pangolin_IOWrapper/PangolinOutput3DWrapper.cpp
  Pangolin_IOWrapper/PangolinOutputIOWrapper.cpp
)

# build shared library.
add_library( lsdgui ${lsdgui_SOURCE_FILES})
target_link_libraries( lsdgui lsdslam )
