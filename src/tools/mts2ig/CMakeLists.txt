CPMAddPackage(
    NAME tinyparsermitsuba
    GITHUB_REPOSITORY PearCoding/TinyParser-Mitsuba 
    GIT_TAG master
    EXCLUDE_FROM_ALL
)

################################################################

add_executable(mts2ig main.cpp SpectralMapper.cpp SpectralMapper.h)
target_link_libraries(mts2ig PRIVATE tinyparser_mitsuba)
install(TARGETS mts2ig COMPONENT tools)