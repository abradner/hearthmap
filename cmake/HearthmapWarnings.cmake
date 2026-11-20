# hearthmap_strict_warnings(<target>)
#
# Warnings as errors. Applied to core and its tests (AGENTS.md §6). Not applied to third-party
# code (LVGL, doctest), whose warnings are not ours to fix.
function(hearthmap_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive-)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic
      -Wconversion -Wshadow -Wnon-virtual-dtor -Wold-style-cast
      -Wcast-align -Wunused -Woverloaded-virtual -Wnull-dereference
      -Wdouble-promotion -Wimplicit-fallthrough
      -Werror)
  endif()
endfunction()
