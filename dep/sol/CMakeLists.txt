add_library(sol INTERFACE)
# sol includes itself with #include<sol/x.hpp>
target_include_directories(sol INTERFACE ../)