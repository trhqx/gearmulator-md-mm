cmake_minimum_required(VERSION 3.15)

add_subdirectory(mdLib)

if(BUILD_TESTING)
	add_subdirectory(mdLibTest)

	# This policy test deliberately stays independent of JUCE so pull requests
	# exercise it in the fast headless configuration as well as release builds.
	add_executable(mdShiftPanelLatchTest
		mdJucePlugin/mdShiftPanelLatchTest.cpp)
	target_link_libraries(mdShiftPanelLatchTest PRIVATE mdLib)
	target_include_directories(mdShiftPanelLatchTest PRIVATE mdJucePlugin)
	add_test(NAME mdShiftPanelLatchTest COMMAND mdShiftPanelLatchTest)
	set_tests_properties(mdShiftPanelLatchTest PROPERTIES LABELS "UnitTest;Panel")
	set_property(TARGET mdShiftPanelLatchTest PROPERTY FOLDER "Elektron/test")

	add_executable(mdPanelHostInputTest
		mdJucePlugin/mdPanelHostInputTest.cpp)
	find_package(Threads REQUIRED)
	target_link_libraries(mdPanelHostInputTest PRIVATE mdLib Threads::Threads)
	target_include_directories(mdPanelHostInputTest PRIVATE mdJucePlugin)
	add_test(NAME mdPanelHostInputTest COMMAND mdPanelHostInputTest)
	set_tests_properties(mdPanelHostInputTest PROPERTIES LABELS "UnitTest;Panel" TIMEOUT 30)
	set_property(TARGET mdPanelHostInputTest PROPERTY FOLDER "Elektron/test")
endif()

if(${CMAKE_PROJECT_NAME}_BUILD_JUCEPLUGIN)
	add_subdirectory(mdJucePlugin)
endif()

include(optimization.cmake)
