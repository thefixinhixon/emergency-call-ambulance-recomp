# Adds a per-game input profile to the run directory's Supermodel.ini without
# touching anything the user already configured.
#   cmake -DINI=<Supermodel.ini> -DPROFILE=<profile.ini> -DSECTION=<name> -P install_profile.cmake
if(EXISTS "${INI}")
  file(READ "${INI}" current)
  string(FIND "${current}" "[ ${SECTION} ]" found)
  if(found EQUAL -1)
    file(READ "${PROFILE}" profile)
    file(APPEND "${INI}" "${profile}")
    message(STATUS "Added [ ${SECTION} ] input profile to ${INI}")
  endif()
else()
  # Supermodel only writes its default file when none exists; its built-in
  # defaults still apply to everything this file does not set.
  configure_file("${PROFILE}" "${INI}" COPYONLY)
endif()
