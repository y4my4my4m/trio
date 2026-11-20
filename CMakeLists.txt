cmake_minimum_required(VERSION 3.16)
project(trio C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(SDL2 REQUIRED)
#A static SDL2 (vcpkg x64-windows-static) exports SDL2::SDL2-static only.
if(NOT TARGET SDL2::SDL2 AND TARGET SDL2::SDL2-static)
  add_library(SDL2::SDL2 INTERFACE IMPORTED)
  target_link_libraries(SDL2::SDL2 INTERFACE SDL2::SDL2-static)
endif()
#Static libraries are also linked into the research library (tools/snespy.c).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/W3)
  add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(snes STATIC
  src/snes/snes.c
  src/snes/cpu.c
  src/snes/ppu.c
  src/snes/apu.c
  src/snes/dsp.c
  src/snes/cx4.c
  src/snes/romdir.c
)
target_include_directories(snes PUBLIC src)
if(NOT MSVC)
  target_link_libraries(snes PUBLIC m)
endif()

option(TRIO_RECOMP "Recompile the ROMs' 65816 code to C at build time" ON)
#The games' ROMs in rom/: gN.sfc, or any .sfc / .smc file there, told
#apart by their headers (tools/find_roms.py).
set(_rom_default_1 "${CMAKE_SOURCE_DIR}/rom/g1.sfc")
set(_rom_default_2 "${CMAKE_SOURCE_DIR}/rom/g2.sfc")
set(_rom_default_3 "${CMAKE_SOURCE_DIR}/rom/g3.sfc")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/find_roms.py ${CMAKE_SOURCE_DIR}/rom
                  OUTPUT_VARIABLE _found_roms OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  string(REPLACE "\n" ";" _found_roms "${_found_roms}")
  foreach(_line IN LISTS _found_roms)
    if(_line MATCHES "^([123]) (.+)$")
      set(_rom_default_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
  endforeach()
endif()
set(G1_ROM "${_rom_default_1}" CACHE FILEPATH "game 1 (USA) ROM")
set(G2_ROM "${_rom_default_2}" CACHE FILEPATH "game 2 (USA) ROM")
set(G3_ROM "${_rom_default_3}" CACHE FILEPATH "game 3 (USA) ROM")
foreach(_g 1 2 3)
  #A cached path that is gone (renamed dump): look again.
  if(NOT EXISTS "${G${_g}_ROM}" AND EXISTS "${_rom_default_${_g}}")
    set(G${_g}_ROM "${_rom_default_${_g}}" CACHE FILEPATH "" FORCE)
  endif()
endforeach()

#Hand-written overrides per game (tools/gN/overrides.txt), compiled with
#that game's generated code.
set(G1_GAME_SOURCES src/game/g1_ws.c)
set(G2_GAME_SOURCES src/game/g2_ws.c)
set(G3_GAME_SOURCES src/game/widescreen.c)

#Recompiled game code, generated from each ROM present at build time (never
#committed), one set per game with its own symbol prefix:
#  tools/disasm.py -> ${CMAKE_BINARY_DIR}/disasm/gN/map.json
#  tools/recomp.py -> src/game/gen/gN/{funcs.h,dispatch.c,code_0..7.c}
#A game without a ROM gets an empty table and runs in the interpreter.
set(TRIO_GAME_SOURCES "")
set(TRIO_GAME_DEFS "")
if(TRIO_RECOMP)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
foreach(g 1 2 3)
  set(rom "${G${g}_ROM}")
  if(NOT TRIO_RECOMP OR NOT EXISTS "${rom}")
    continue()
  endif()
  set(map "${CMAKE_BINARY_DIR}/disasm/g${g}/map.json")
  set(gen "${CMAKE_SOURCE_DIR}/src/game/gen/g${g}")
  set(tdir "tools/g${g}")
  set(dargs --rom "${rom}" --hints ${tdir}/hints.txt --out "${CMAKE_BINARY_DIR}/disasm/g${g}" -q)
  set(ddeps tools/disasm.py ${tdir}/hints.txt "${rom}")
  if(EXISTS "${CMAKE_SOURCE_DIR}/${tdir}/coverage.txt")
    list(APPEND dargs --trace ${tdir}/coverage.txt)
    list(APPEND ddeps ${tdir}/coverage.txt)
  endif()
  add_custom_command(OUTPUT "${map}"
    COMMAND ${Python3_EXECUTABLE} tools/disasm.py ${dargs}
    DEPENDS ${ddeps}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Disassembling g${g}"
    VERBATIM)
  set(srcs "${gen}/dispatch.c")
  foreach(i RANGE 7)
    list(APPEND srcs "${gen}/code_${i}.c")
  endforeach()
  add_custom_command(OUTPUT ${srcs} "${gen}/funcs.h"
    COMMAND ${Python3_EXECUTABLE} tools/recomp.py --map "${map}" --rom "${rom}" --hints ${tdir}/hints.txt
            --out "${gen}" --overrides ${tdir}/overrides.txt --prefix g${g}
    DEPENDS tools/recomp.py tools/disasm.py ${tdir}/hints.txt ${tdir}/overrides.txt "${rom}" "${map}"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Recompiling g${g} 65816 -> C"
    VERBATIM)
  if(NOT MSVC)
    set_source_files_properties(${srcs} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable")
  endif()
  list(APPEND TRIO_GAME_SOURCES ${srcs} ${G${g}_GAME_SOURCES})
  list(APPEND TRIO_GAME_DEFS TRIO_HAVE_G${g}=1)
endforeach()
add_library(triogame STATIC src/game/recomp_rt.c src/game/ws_rom.c src/game/game_stubs.c src/game/port_menu.c src/game/battle_hooks.c ${TRIO_GAME_SOURCES})
target_compile_definitions(triogame PRIVATE ${TRIO_GAME_DEFS})
#Settings and the frontend service table, shared by game code and frontend.
add_library(triocfg STATIC src/port/settings.c src/port/port_api.c)
target_include_directories(triocfg PUBLIC src)
target_link_libraries(triocfg PUBLIC SDL2::SDL2)
target_include_directories(triogame PUBLIC src)
target_link_libraries(triogame PUBLIC snes triocfg)

add_library(trioscript STATIC src/script.c)
target_include_directories(trioscript PUBLIC src)
target_link_libraries(trioscript PUBLIC snes)

set(CMAKE_CXX_STANDARD 17)
add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_sdl2.cpp
  third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_link_libraries(imgui PUBLIC SDL2::SDL2)
if(NOT MSVC)
  target_compile_options(imgui PRIVATE -w)
endif()

#Frontend features: settings, widescreen/input glue, rewind, save slots, menu.
add_library(trioport STATIC src/port/enhance.c src/port/rewind.c
  src/port/saveslots.c src/port/saveslots_ui.cpp src/port/overlay.cpp)
target_include_directories(trioport PUBLIC src)
target_link_libraries(trioport PUBLIC triogame snes imgui SDL2::SDL2)

#WIN32: GUI subsystem on Windows (no console window); SDL2main supplies WinMain.
add_executable(trio WIN32 src/main.c)
target_link_libraries(trio PRIVATE trioport trioarena triogame trioscript snes SDL2::SDL2)
if(TARGET SDL2::SDL2main)
  target_link_libraries(trio PRIVATE SDL2::SDL2main)
endif()

add_executable(trio-verify tools/verify.c)
target_link_libraries(trio-verify PRIVATE triogame trioscript snes)

enable_testing()
foreach(t cpu ppu apu cx4)
  add_executable(${t}_test tests/${t}_test.c)
  target_link_libraries(${t}_test PRIVATE snes)
  add_test(NAME ${t} COMMAND ${t}_test)
endforeach()
add_executable(rewind_test tests/rewind_test.c src/port/rewind.c)
target_include_directories(rewind_test PRIVATE src)
target_link_libraries(rewind_test PRIVATE snes)
add_test(NAME rewind COMMAND rewind_test)

#Research tool: run a state + input script and dump WRAM/OAM at chosen frames.
add_executable(trio-probe tools/probe.c)
target_link_libraries(trio-probe PRIVATE triogame trioscript snes)

#Battle mode: one copy of the game per fighter (docs/BATTLE.md).
add_library(trioarena STATIC src/arena/ui.c src/arena/launcher.c src/arena/games.c src/arena/trio.c src/arena/stage.c
  src/arena/sound.c src/arena/sprites.c src/arena/boss.c src/arena/arena.c src/arena/abot.c src/arena/battle.c)
target_include_directories(trioarena PUBLIC src)
target_link_libraries(trioarena PUBLIC triogame snes)

add_executable(trio_test tests/trio_test.c)
target_link_libraries(trio_test PRIVATE trioarena)
add_test(NAME trio COMMAND trio_test ${CMAKE_SOURCE_DIR}/rom)
set_tests_properties(trio PROPERTIES SKIP_RETURN_CODE 77)

add_executable(sound_test tests/sound_test.c)
target_link_libraries(sound_test PRIVATE trioarena)
add_test(NAME sound COMMAND sound_test ${CMAKE_SOURCE_DIR}/rom)
set_tests_properties(sound PROPERTIES SKIP_RETURN_CODE 77)

add_executable(arena_test tests/arena_test.c)
target_link_libraries(arena_test PRIVATE trioarena)
add_test(NAME arena COMMAND arena_test ${CMAKE_SOURCE_DIR}/rom 3)
set_tests_properties(arena PROPERTIES SKIP_RETURN_CODE 77)

#Headless battle mode run from the title menu, saving frames.
add_executable(trio-battle-shot tools/battle_shot.c)
target_link_libraries(trio-battle-shot PRIVATE trioarena)

#Research library for tools/sp.py (Python ctypes): any of the three games,
#breakpoints, WRAM write watches. Not part of the port.
add_library(snespy SHARED tools/snespy.c)
target_link_libraries(snespy PRIVATE triogame snes)

#Battle arenas as the battle view shows them (tools/arena_preview.c).
add_executable(trio-arena-preview tools/arena_preview.c)
target_link_libraries(trio-arena-preview PRIVATE trioarena)

#Balance check: CPU against CPU for every pair of characters (tools/balance.c).
add_executable(trio-balance tools/balance.c)
target_link_libraries(trio-balance PRIVATE trioarena)
