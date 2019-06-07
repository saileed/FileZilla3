#include <filezilla.h>

#include <libfilezilla/format.hpp>
#include <libfilezilla/time.hpp>
#include <libfilezilla/tls_layer.hpp>

#include <random>
#include <cstdint>
#include <cwctype>

#include <string.h>

std::wstring GetDependencyVersion(lib_dependency d)
{
	switch (d) {
	case lib_dependency::gnutls:
		return fz::to_wstring(fz::tls_layer::get_gnutls_version());
	default:
		return std::wstring();
	}
}

std::wstring GetDependencyName(lib_dependency d)
{
	switch (d) {
	case lib_dependency::gnutls:
		return L"GnuTLS";
	default:
		return std::wstring();
	}
}

std::string ListTlsCiphers(std::string const& priority)
{
	return fz::tls_layer::list_tls_ciphers(priority);
}

#if FZ_WINDOWS
DWORD GetSystemErrorCode()
{
	return GetLastError();
}

std::wstring GetSystemErrorDescription(DWORD err)
{
	wchar_t* buf{};
	if (!FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<wchar_t*>(&buf), 0, nullptr) || !buf) {
		return fz::sprintf(_("Unknown error %u"), err);
	}
	std::wstring ret = buf;
	LocalFree(buf);

	return ret;
}
#else
int GetSystemErrorCode()
{
	return errno;
}

namespace {
inline std::string ProcessStrerrorResult(int ret, char* buf, int err)
{
	// For XSI strerror_r
	std::string s;
	if (!ret) {
		buf[999] = 0;
		s = buf;
	}
	else {
		s = fz::to_string(fz::sprintf(_("Unknown error %d"), err));
	}
	return s;
}

inline std::string ProcessStrerrorResult(char* ret, char*, int err)
{
	// For GNU strerror_r
	std::string s;
	if (ret) {
		s = ret;
	}
	else {
		s = fz::to_string(fz::sprintf(_("Unknown error %d"), err));
	}
	return s;
}
}

std::string GetSystemErrorDescription(int err)
{
	char buf[1000];
	auto ret = strerror_r(err, buf, 1000);
	return ProcessStrerrorResult(ret, buf, err);
}
#endif

namespace fz {

std::wstring str_tolower(std::wstring const& source)
{
	std::wstring ret;
	ret.reserve(source.size());
	for (auto const& c : source) {
		ret.push_back(std::towlower(c));
	}
	return ret;
}

}
