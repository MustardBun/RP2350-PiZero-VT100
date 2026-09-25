add_library(libgui INTERFACE)
target_sources(libgui INTERFACE
	${CMAKE_CURRENT_LIST_DIR}/Debug.h
	${CMAKE_CURRENT_LIST_DIR}/fonts.h
	${CMAKE_CURRENT_LIST_DIR}/font8.c
	${CMAKE_CURRENT_LIST_DIR}/font12.c
	${CMAKE_CURRENT_LIST_DIR}/font12CN.c
	${CMAKE_CURRENT_LIST_DIR}/font16.c
	${CMAKE_CURRENT_LIST_DIR}/font20.c
	${CMAKE_CURRENT_LIST_DIR}/font24.c
	${CMAKE_CURRENT_LIST_DIR}/font24CN.c
	${CMAKE_CURRENT_LIST_DIR}/GUI_Paint.c
	${CMAKE_CURRENT_LIST_DIR}/GUI_Paint.h
	)


target_include_directories(libgui INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(libgui INTERFACE pico_base_headers)