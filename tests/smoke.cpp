#include <string_view>

int main()
{
    constexpr std::string_view name = "ovrwin";
    return name.empty() ? 1 : 0;
}
