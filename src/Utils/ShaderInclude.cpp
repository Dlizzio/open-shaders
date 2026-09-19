#include "ShaderInclude.h"

#include <algorithm>
#include <format>
#include <limits>
#include <mutex>
#include <spdlog/spdlog.h>
#include <system_error>
#include <unordered_set>
#include <winrt/base.h>

namespace Util::ShaderInclude
{
	bool Read(const std::filesystem::path& path, File& contents, ReadError& error) noexcept
	{
		contents = {};
		error = {};
		winrt::file_handle handle{ CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
		if (!handle) {
			error = { "open", GetLastError() };
			return false;
		}

		LARGE_INTEGER size;
		if (!GetFileSizeEx(handle.get(), &size)) {
			error = { "size", GetLastError() };
			return false;
		}
		if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > std::numeric_limits<UINT>::max()) {
			error = { "size", ERROR_FILE_TOO_LARGE, static_cast<uint64_t>(size.QuadPart) };
			return false;
		}

		File result;
		result.size = static_cast<UINT>(size.QuadPart);
		try {
			result.data = std::make_unique_for_overwrite<char[]>(std::max<size_t>(result.size, 1));
		} catch (const std::bad_alloc&) {
			error = { "allocate", ERROR_NOT_ENOUGH_MEMORY, result.size };
			return false;
		}

		UINT totalRead = 0;
		while (totalRead < result.size) {
			DWORD bytesRead = 0;
			if (!ReadFile(handle.get(), result.data.get() + totalRead, result.size - totalRead, &bytesRead, nullptr)) {
				error = { "read", GetLastError(), result.size, totalRead };
				return false;
			}
			if (bytesRead == 0) {
				error = { "read", ERROR_HANDLE_EOF, result.size, totalRead };
				return false;
			}
			totalRead += bytesRead;
		}
		contents = std::move(result);
		return true;
	}

	void Report(const std::filesystem::path& source, const char* loader, const char* include,
		const std::filesystem::path& attemptedPath, const ReadError& error) noexcept
	{
		try {
			constexpr size_t kMaxReports = 128;
			static std::mutex mutex;
			static std::unordered_set<std::string> reported;
			const auto key = std::format("{}|{}|{}|{}|{}", loader, source.string(), attemptedPath.string(), error.operation, error.code);
			{
				std::lock_guard lock(mutex);
				if (reported.size() >= kMaxReports || !reported.insert(key).second)
					return;
			}
			std::error_code cwdError;
			const auto cwd = std::filesystem::current_path(cwdError);
			std::error_code pathError;
			const auto absolute = std::filesystem::absolute(attemptedPath, pathError);
			spdlog::error(
				"[ShaderIncludeIO v2] loader={} source='{}' include='{}' operation={} attempted='{}' "
				"absolute_at_report='{}' cwd_at_report='{}' win32_error={} ({}) expected_bytes={} read_bytes={} "
				"cwd_error={} absolute_error={}. Duplicate diagnostics suppressed until process exit; maximum {} distinct reports.",
				loader, source.string(), include, error.operation, attemptedPath.string(), absolute.string(), cwd.string(), error.code,
				std::system_category().message(static_cast<int>(error.code)), error.expectedBytes, error.readBytes,
				cwdError.value(), pathError.value(), kMaxReports);
		} catch (...) {
			OutputDebugStringA("[ShaderIncludeIO v2] Unable to format include failure diagnostics.\n");
		}
	}
}

namespace Util
{
	HRESULT CustomInclude::Open([[maybe_unused]] D3D_INCLUDE_TYPE type, LPCSTR filename,
		[[maybe_unused]] LPCVOID parent, LPCVOID* data, UINT* size)
	{
		*data = nullptr;
		*size = 0;
		try {
			const auto path = std::filesystem::path(L"Data\\Shaders") / filename;
			ShaderInclude::File contents;
			ShaderInclude::ReadError error;
			if (!ShaderInclude::Read(path, contents, error)) {
				ShaderInclude::Report(sourcePath, "CustomInclude", filename, path, error);
				return HRESULT_FROM_WIN32(error.code);
			}
			*size = contents.size;
			*data = contents.data.release();
			return S_OK;
		} catch (const std::bad_alloc&) {
			ShaderInclude::Report(sourcePath, "CustomInclude", filename, sourcePath, { "prepare_include", ERROR_NOT_ENOUGH_MEMORY });
			return E_OUTOFMEMORY;
		} catch (...) {
			ShaderInclude::Report(sourcePath, "CustomInclude", filename, sourcePath, { "prepare_include", ERROR_UNHANDLED_EXCEPTION });
			return E_FAIL;
		}
	}

	HRESULT CustomInclude::Close(LPCVOID data)
	{
		delete[] static_cast<const char*>(data);
		return S_OK;
	}
}
