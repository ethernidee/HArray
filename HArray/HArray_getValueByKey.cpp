/*
# Copyright(C) 2010-2017 Viacheslav Makoveichuk (email: slv709@gmail.com, skype: vyacheslavm81)
# This file is part of HArray.
#
# HArray is free software : you can redistribute it and / or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# HArray is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Foobar.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "HArray.h"

uint32 HArray::getValueByKey(uint32* key,
	uint32 keyLen,
	uchar8& valueType,
	uchar8 readModeType,
	uchar8 tranID,
	ReadList* pReadList)
{
	keyLen >>= 2; //in 4 bytes

	uint32 headerOffset;

	if (!normalizeFunc)
	{
		headerOffset = key[0] >> HeaderBits;
	}
	else
	{
		headerOffset = (*normalizeFunc)(key);
	}

	uint32 contentOffset = pHeader[headerOffset];

	if (contentOffset)
	{
		uint32 keyOffset = 0;

	NEXT_KEY_PART:
		ContentPage* pContentPage = pContentPages[contentOffset >> 16];
		ushort16 contentIndex = contentOffset & 0xFFFF;

		uchar8 contentCellType = pContentPage->pType[contentIndex]; //move to type part

		if (contentCellType >= ONLY_CONTENT_TYPE) //ONLY CONTENT =========================================================================================
		{
			if ((keyLen - keyOffset) != (contentCellType - ONLY_CONTENT_TYPE))
			{
				return 0;
			}

			for (; keyOffset < keyLen; contentIndex++, keyOffset++)
			{
				if (pContentPage->pContent[contentIndex] != key[keyOffset])
					return 0;
			}

			valueType = pContentPage->pType[contentIndex];

			if (readModeType)
			{
				return processReadByTranID(&pContentPage->pContent[contentIndex],
										   &pContentPage->ReadByTranID[contentIndex],
										   readModeType,
										   tranID,
										   pReadList);
			}
			else
			{
				return pContentPage->pContent[contentIndex]; //return value
			}
		}

		uint32& keyValue = key[keyOffset];
		uint32* pContentCellValueOrOffset = &pContentPage->pContent[contentIndex];
		std::atomic<uchar8>* pContentCellReadByTranID = &pContentPage->ReadByTranID[contentIndex];

		if (contentCellType == VAR_TYPE) //VAR =====================================================================
		{
			VarPage* pVarPage = pVarPages[(*pContentCellValueOrOffset) >> 16];
			VarCell& varCell = pVarPage->pVar[(*pContentCellValueOrOffset) & 0xFFFF];

			if (keyOffset < keyLen)
			{
				contentCellType = varCell.ContCellType; //read from var cell

				if (contentCellType == CONTINUE_VAR_TYPE) //CONTINUE VAR =====================================================================
				{
					contentOffset = varCell.ContCellValue;

					goto NEXT_KEY_PART;
				}
				else
				{
					pContentCellReadByTranID = &varCell.ContCellReadByTranID;
					pContentCellValueOrOffset = &varCell.ContCellValue;
				}
			}
			else
			{
				if (varCell.ValueContCellType)
				{
					valueType = varCell.ValueContCellType;

					if (readModeType)
					{
						return processReadByTranID(&varCell.ValueContCellValue,
							&varCell.ValueContCellReadByTranID,
							readModeType,
							tranID,
							pReadList);
					}
					else
					{
						return varCell.ValueContCellValue;
					}
				}
				else
				{
					return 0;
				}
			}
		}
		else if (keyOffset == keyLen)
		{
			if (contentCellType == VALUE_TYPE ||
				contentCellType == VALUE_LIST_TYPE)
			{
				valueType = contentCellType;

				if (readModeType)
				{
					return processReadByTranID(pContentCellValueOrOffset,
						pContentCellReadByTranID,
						readModeType,
						tranID,
						pReadList);
				}
				else
				{
					return *pContentCellValueOrOffset;
				}
			}
			else
			{
				return 0;
			}
		}

		if (contentCellType <= MAX_BRANCH_TYPE1) //BRANCH =====================================================================
		{
			BranchPage* pBranchPage = pBranchPages[(*pContentCellValueOrOffset) >> 16];
			BranchCell& branchCell = pBranchPage->pBranch[(*pContentCellValueOrOffset) & 0xFFFF];

			//try find value in the list
			uint32* values = branchCell.Values;

			for (uint32 i = 0; i < contentCellType; i++)
			{
				if (values[i] == keyValue)
				{
					contentOffset = branchCell.Offsets[i];
					keyOffset++;

					goto NEXT_KEY_PART;
				}
			}

			return 0;
		}
		else if (contentCellType == VALUE_TYPE ||
			contentCellType == VALUE_LIST_TYPE)
		{
			if (keyOffset == keyLen)
			{
				valueType = contentCellType;

				if (readModeType)
				{
					return processReadByTranID(pContentCellValueOrOffset,
											   pContentCellReadByTranID,
											   readModeType,
											   tranID,
											   pReadList);
				}
				else
				{
					return *pContentCellValueOrOffset;
				}
			}
			else
			{
				return 0;
			}
		}
		else if (contentCellType <= MAX_BLOCK_TYPE) //VALUE IN BLOCK ===================================================================
		{
			uchar8 idxKeyValue = (contentCellType - MIN_BLOCK_TYPE) * BLOCK_ENGINE_STEP;

			uint32 startOffset = *pContentCellValueOrOffset;

		NEXT_BLOCK:
			uint32 subOffset = ((keyValue << idxKeyValue) >> BLOCK_ENGINE_SHIFT);
			uint32 blockOffset = startOffset + subOffset;

			BlockPage* pBlockPage = pBlockPages[blockOffset >> 16];
			BlockCell& blockCell = pBlockPage->pBlock[blockOffset & 0xFFFF];

			uchar8& blockCellType = blockCell.Type;

			if (blockCellType == EMPTY_TYPE)
			{
				return 0;
			}
			else if (blockCellType == CURRENT_VALUE_TYPE) //current value
			{
				if (blockCell.ValueOrOffset == keyValue) //value is exists
				{
					contentOffset = blockCell.Offset;
					keyOffset++;

					goto NEXT_KEY_PART;
				}
				else
				{
					return 0;
				}
			}
			else if (blockCellType <= MAX_BRANCH_TYPE1) //branch cell
			{
				BranchCell& branchCell1 = pBranchPages[blockCell.Offset >> 16]->pBranch[blockCell.Offset & 0xFFFF];

				//try find value in the list
				for (uint32 i = 0; i < blockCellType; i++)
				{
					if (branchCell1.Values[i] == keyValue)
					{
						contentOffset = branchCell1.Offsets[i];
						keyOffset++;

						goto NEXT_KEY_PART;
					}
				}

				return 0;
			}
			else if (blockCellType <= MAX_BRANCH_TYPE2) //branch cell
			{
				BranchCell& branchCell1 = pBranchPages[blockCell.Offset >> 16]->pBranch[blockCell.Offset & 0xFFFF];

				//try find value in the list
				for (uint32 i = 0; i < BRANCH_ENGINE_SIZE; i++)
				{
					if (branchCell1.Values[i] == keyValue)
					{
						contentOffset = branchCell1.Offsets[i];
						keyOffset++;

						goto NEXT_KEY_PART;
					}
				}

				BranchCell& branchCell2 = pBranchPages[blockCell.ValueOrOffset >> 16]->pBranch[blockCell.ValueOrOffset & 0xFFFF];

				//try find value in the list
				uint32 countValues = blockCellType - MAX_BRANCH_TYPE1;

				for (uint32 i = 0; i < countValues; i++)
				{
					if (branchCell2.Values[i] == keyValue)
					{
						contentOffset = branchCell2.Offsets[i];
						keyOffset++;

						goto NEXT_KEY_PART;
					}
				}

				return 0;
			}
			else if (blockCell.Type <= MAX_BLOCK_TYPE)
			{
				//go to block
				idxKeyValue = (blockCell.Type - MIN_BLOCK_TYPE) * BLOCK_ENGINE_STEP;
				startOffset = blockCell.Offset;

				goto NEXT_BLOCK;
			}
			else
			{
				return 0;
			}
		}
		else if (contentCellType == CURRENT_VALUE_TYPE) //PART OF KEY =========================================================================
		{
			if (*pContentCellValueOrOffset == keyValue)
			{
				contentOffset++;
				keyOffset++;

				goto NEXT_KEY_PART;
			}
			else
			{
				return 0;
			}
		}
	}

	return 0;
}
