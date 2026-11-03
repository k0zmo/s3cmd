# Embed the existing ICO assets without an external resource compiler. Track
# them as configure dependencies so regenerating an icon updates the module.
function(s3cmd_embed_icons output)
    set(content "#pragma once\n\nnamespace s3cmd::embedded_icons {\n")
    foreach(name IN ITEMS profile bucket)
        set(path "${PROJECT_SOURCE_DIR}/icons/${name}.ico")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" data HEX)
        string(REGEX REPLACE "(..)" "0x\\1," data "${data}")
        string(APPEND content "static unsigned char ${name}[] = {${data}};\n")
    endforeach()
    string(APPEND content "} // namespace s3cmd::embedded_icons\n")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${content}" @ONLY)
endfunction()
