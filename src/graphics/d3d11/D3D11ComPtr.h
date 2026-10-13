/*
 * Copyright 2026 Arx Libertatis VR contributors <kazakov.s.fl@gmail.com>
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ARX_GRAPHICS_D3D11_D3D11COMPTR_H
#define ARX_GRAPHICS_D3D11_D3D11COMPTR_H

#include <cstddef>
#include <memory>

#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>

// Defined by the COM headers, conflicts with Config::interface
#undef interface

namespace d3d11 {

/*!
 * Minimal COM smart pointer.
 *
 * Microsoft::WRL::ComPtr requires a newer NTDDI_VERSION than Arx Libertatis targets.
 */
template <class T>
class ComPtr {
	
	T * m_ptr = nullptr;
	
public:
	
	ComPtr() noexcept = default;
	ComPtr(std::nullptr_t) noexcept { } // NOLINT
	
	ComPtr(const ComPtr & other) noexcept : m_ptr(other.m_ptr) {
		if(m_ptr) {
			m_ptr->AddRef();
		}
	}
	
	ComPtr(ComPtr && other) noexcept : m_ptr(other.m_ptr) {
		other.m_ptr = nullptr;
	}
	
	~ComPtr() {
		Reset();
	}
	
	ComPtr & operator=(const ComPtr & other) noexcept {
		if(other.m_ptr) {
			other.m_ptr->AddRef();
		}
		Reset();
		m_ptr = other.m_ptr;
		return *this;
	}
	
	ComPtr & operator=(ComPtr && other) noexcept {
		if(this != std::addressof(other)) {
			Reset();
			m_ptr = other.m_ptr;
			other.m_ptr = nullptr;
		}
		return *this;
	}
	
	T * Get() const noexcept { return m_ptr; }
	T * operator->() const noexcept { return m_ptr; }
	explicit operator bool() const noexcept { return m_ptr != nullptr; }
	
	//! Release the current object and return the address for an out parameter
	T ** operator&() noexcept { // NOLINT
		Reset();
		return &m_ptr;
	}
	
	void Reset() noexcept {
		if(m_ptr) {
			m_ptr->Release();
			m_ptr = nullptr;
		}
	}
	
	template <class U>
	HRESULT As(U ** other) const noexcept {
		return m_ptr->QueryInterface(__uuidof(U), reinterpret_cast<void **>(other));
	}
	
};

} // namespace d3d11

#endif // ARX_GRAPHICS_D3D11_D3D11COMPTR_H
