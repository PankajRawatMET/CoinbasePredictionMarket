# Project warning flags. Applied to our own targets only, never to QuickFIX.
function(cpm_set_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()
