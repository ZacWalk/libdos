// Purpose: Verify that using the host interface does not pull the Windows SDK into dui.
#include "host-win32.h"

#ifdef _WINDOWS_
#error The host header must not include the Windows SDK.
#endif

#include <type_traits>

static_assert(std::is_base_of_v<host_services, win32_host>);
static_assert(!std::is_copy_constructible_v<win32_host>);

int main()
{
	return 0;
}
