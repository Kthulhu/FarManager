﻿#ifndef FILESTR_HPP_1B6BCA12_AFF9_4C80_A59C_B4B92B21F83F
#define FILESTR_HPP_1B6BCA12_AFF9_4C80_A59C_B4B92B21F83F
#pragma once

/*
filestr.hpp

Класс GetFileString
*/
/*
Copyright © 1996 Eugene Roshal
Copyright © 2000 Far Group
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "encoding.hpp"
#include "eol.hpp"

class GetFileString: noncopyable
{
public:
	GetFileString(const os::fs::file& SrcFile, uintptr_t CodePage);
	bool PeekString(string_view& Str, eol::type* Eol = nullptr);
	bool GetString(string_view& Str, eol::type* Eol = nullptr);
	bool GetString(string& str, eol::type* Eol = nullptr);
	bool IsConversionValid() const { return !SomeDataLost; }

private:
	template<typename T>
	bool GetTString(std::vector<T>& From, std::vector<T>& To, eol::type* Eol, bool bBigEndian = false);

	const os::fs::file& SrcFile;
	uintptr_t m_CodePage;
	size_t ReadPos{};
	size_t ReadSize{};

	bool Peek{};
	string_view LastStr;
	bool LastResult{};

	std::vector<char> m_ReadBuf;
	std::vector<wchar_t> m_wReadBuf;
	std::vector<wchar_t> m_wStr;

	raw_eol m_Eol;

	bool SomeDataLost{};
	bool m_CrSeen{};
	bool bCrCr{};
};

bool GetFileFormat(const os::fs::file& file, uintptr_t& nCodePage, bool* pSignatureFound = nullptr, bool bUseHeuristics = true, bool* pPureAscii = nullptr);

#endif // FILESTR_HPP_1B6BCA12_AFF9_4C80_A59C_B4B92B21F83F
