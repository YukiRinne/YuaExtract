#pragma once

#include <windows.h>
#include "detours.h"
namespace Engine
{
	namespace HookUtils
	{
		class InlineHook
		{
		public:
			InlineHook() = delete;
			InlineHook(const InlineHook&) = delete;
			InlineHook(InlineHook&&) = delete;
			InlineHook& operator=(const InlineHook&) = delete;
			InlineHook& operator=(InlineHook&&) = delete;
			~InlineHook() = delete;


			template<class T>
			static void Hook(T& OriginalFunction, T DetourFunction)
			{
				DetourUpdateThread(GetCurrentThread());
				DetourTransactionBegin();
				DetourAttach(&(PVOID&)OriginalFunction, (PVOID&)DetourFunction);
				DetourTransactionCommit();
			}

			template<class T>
			static void UnHook(T& OriginalFunction, T DetourFunction)
			{
				DetourUpdateThread(GetCurrentThread());
				DetourTransactionBegin();
				DetourDetach(&(PVOID&)OriginalFunction, (PVOID&)DetourFunction);
				DetourTransactionCommit();
			}
		};
	}

	namespace StreamUtils
	{
		class IStreamEx
		{
		public:
			IStreamEx() = delete;
			IStreamEx(const IStreamEx&) = delete;
			IStreamEx(IStreamEx&&) = delete;
			IStreamEx& operator=(const IStreamEx&) = delete;
			IStreamEx& operator=(IStreamEx&&) = delete;
			~IStreamEx() = delete;

			// 获取流长度
			static ULONGLONG WINAPI Length(IStream* stream)
			{
				LARGE_INTEGER pos{ };
				pos.QuadPart = (LONGLONG)IStreamEx::Position(stream);

				ULARGE_INTEGER size;
				stream->Seek(LARGE_INTEGER{ }, STREAM_SEEK_END, &size);
				stream->Seek(pos, STREAM_SEEK_SET, nullptr);

				return size.QuadPart;
			}
			// 获取当前流位置
			static ULONGLONG WINAPI Position(IStream* stream)
			{
				ULARGE_INTEGER pos;
				stream->Seek(LARGE_INTEGER{ }, STREAM_SEEK_CUR, &pos);
				return pos.QuadPart;
			}
			// 设置当前流位置
			static void WINAPI Seek(IStream* stream, LONGLONG offset, DWORD seekMode)
			{
				LARGE_INTEGER move{ };
				move.QuadPart = offset;
				stream->Seek(move, seekMode, nullptr);
			}
			// 读取流
			static ULONG WINAPI Read(IStream* stream, void* buffer, ULONG length)
			{
				ULONG readLength = 0ul;
				stream->Read(buffer, length, &readLength);
				return readLength;
			}

			// ---- 带失败判定的版本 ----
			// 旧版本把 Seek/Read 的 HRESULT 与"实际读了多少"全丢了：短读会被当成读满，
			// 后续逻辑拿零填的缓冲区继续跑，最后把一份坏文件当成功写出去。
			static bool WINAPI SeekEx(IStream* stream, LONGLONG offset, DWORD seekMode,
			                          ULARGE_INTEGER* newPosition = nullptr)
			{
				if (stream == nullptr)
				{
					return false;
				}
				LARGE_INTEGER move{ };
				move.QuadPart = offset;
				ULARGE_INTEGER local{ };
				const HRESULT hr = stream->Seek(move, seekMode, newPosition != nullptr ? newPosition : &local);
				// COM IStream::Seek 只有 S_OK 表示"确实到了指定位置"；S_FALSE 等不能当成功。
				return hr == S_OK;
			}

			// 只有"读满请求长度"才算成功。EOF 提前到、READ 返回 S_FALSE、直接失败都算短读。
			static bool WINAPI ReadExact(IStream* stream, void* buffer, ULONG length, ULONG* actual = nullptr)
			{
				if (actual != nullptr)
				{
					*actual = 0ul;
				}
				if (stream == nullptr || (buffer == nullptr && length != 0ul))
				{
					return false;
				}
				ULONG readLength = 0ul;
				const HRESULT hr = stream->Read(buffer, length, &readLength);
				if (actual != nullptr)
				{
					*actual = readLength;
				}
				return SUCCEEDED(hr) && hr != S_FALSE && readLength == length;
			}

			static bool WINAPI PositionEx(IStream* stream, ULONGLONG& position)
			{
				if (stream == nullptr)
				{
					return false;
				}
				ULARGE_INTEGER pos{ };
				if (!SeekEx(stream, 0, STREAM_SEEK_CUR, &pos))
				{
					return false;
				}
				position = pos.QuadPart;
				return true;
			}

			static bool WINAPI LengthEx(IStream* stream, ULONGLONG& length)
			{
				if (stream == nullptr)
				{
					return false;
				}
				ULONGLONG start = 0;
				if (!PositionEx(stream, start))
				{
					return false;
				}
				ULARGE_INTEGER size{ };
				if (!SeekEx(stream, 0, STREAM_SEEK_END, &size))
				{
					return false;
				}
				LARGE_INTEGER back{ };
				back.QuadPart = static_cast<LONGLONG>(start);
				if (!SeekEx(stream, back.QuadPart, STREAM_SEEK_SET))
				{
					return false;
				}
				length = size.QuadPart;
				return true;
			}
		};
	}
}



