/******************************************************************************
*
* Copyright (C) 2017 - 2019 Xilinx, Inc.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* XILINX  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
* OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*
* Except as contained in this notice, the name of the Xilinx shall not be used
* in advertising or otherwise to promote the sale, use or other dealings in
* this Software without prior written authorization from Xilinx.
*
******************************************************************************/
/*****************************************************************************/
/**
*
* @file xsecure.c
*
* This file contains the implementation of the interface functions for secure
* library.
*
* <pre>
* MODIFICATION HISTORY:
*
* Ver   Who Date     Changes
* ----- --- -------- -------------------------------------------------------
* 1.0   DP  02/15/17 Initial release
* 2.2   vns 09/18/17 Added APIs to support generic functionality
*                    for SHA3 and RSA hardware at linux level.
*                    Removed RSA-SHA2 authentication support
*                    for loading linux image and dtb from u-boot, as here it
*                    is using SHA2 hash and single RSA key pair authentication
* 3.0   vns 02/21/18 Added support for single partition image authentication
*                    and/or decryption.
* 3.1   vns 04/13/18 Added device key support even if authentication is not
*                    been enabled for single partition image, when PMUFW is
*                    compiled by enabling secure environment variable in bsp
*                    settings.
*       ka  04/10/18 Added support for user-efuse revocation
*       ka  04/18/18 Added support for Zeroization of the memory in case of
*                    Gcm-Tag mismatch
* 3.2   ka  04/04/18 Added support for Sha3_Update, if the payload is not
*        	     4 bytes aligned.
*       ka  08/03/18 Added XSecure_Aes Api's to encrypt or decrypt data-blobs.
*       ka  10/25/18 Added support to clear user key after use.
* 4.0   arc 18/12/18 Fixed MISRA-C violations.
*       arc 12/02/19 Added support for validate image format.
*
* </pre>
*
* @note
*
******************************************************************************/

/***************************** Include Files *********************************/

#include "xsecure.h"
#include "xparameters.h"

static XSecure_Aes SecureAes;
static XSecure_Rsa Secure_Rsa;
static XSecure_Sha3 Sha3Instance;

XCsuDma CsuDma = {0U};

/************************** Function Prototypes ******************************/

/************************** Function Definitions *****************************/

/*****************************************************************************/
/**
 * This function is used to initialize the DMA driver
 *
 * @param	None
 *
 * @return	returns the error code on any error
 *		returns XST_SUCCESS on success
 *
 *****************************************************************************/
u32 XSecure_CsuDmaInit(void)
{
	u32 Status;
	s32 SStatus;
	XCsuDma_Config * CsuDmaConfig;

	CsuDmaConfig = XCsuDma_LookupConfig(XSECURE_CSUDMA_DEVICEID);
	if (NULL == CsuDmaConfig) {
		Status = XSECURE_ERROR_CSUDMA_INIT_FAIL;
		goto END;
	}

	SStatus = XCsuDma_CfgInitialize(&CsuDma, CsuDmaConfig,
			CsuDmaConfig->BaseAddress);
	if (SStatus != XST_SUCCESS) {
		Status = XSECURE_ERROR_CSUDMA_INIT_FAIL;
		goto END;
	}
	Status = (u32)XST_SUCCESS;
END:
	return Status;
}

/****************************************************************************/
/**
 * Converts the char into the equivalent nibble.
 *	Ex: 'a' -> 0xa, 'A' -> 0xa, '9'->0x9
 *
 * @param	InChar is input character. It has to be between 0-9,a-f,A-F
 * @param	Num is the output nibble.
 *
 * @return
 *		- XST_SUCCESS no errors occured.
 *		- ERROR when input parameters are not valid
 *
 * @note	None.
 *
 *****************************************************************************/
static u32 XSecure_ConvertCharToNibble(char InChar, u8 *Num) {

	u32 Status = XST_SUCCESS;

	/* Convert the char to nibble */
	if ((InChar >= '0') && (InChar <= '9')) {
		*Num = (u8)InChar - (u8)'0';
	}
	else if ((InChar >= 'a') && (InChar <= 'f')) {
		*Num = (u8)InChar - (u8)'a' + (u8)10U;
	}
	else if ((InChar >= 'A') && (InChar <= 'F')) {
		*Num = (u8)InChar - (u8)'A' + (u8)10U;
	}
	else {
		Status = XSECURE_STRING_INVALID_ERROR;
		goto END;
	}
END:
	return Status;
}

/****************************************************************************/
/**
 * Converts the string into the equivalent Hex buffer.
 *	Ex: "abc123" -> {0xab, 0xc1, 0x23}
 *
 * @param	Str is a Input String. Will support the lower and upper
 *		case values. Value should be between 0-9, a-f and A-F
 *
 * @param	Buf is Output buffer.
 * @param	Len of the input string. Should have even values
 *
 * @return
 *		- XST_SUCCESS no errors occured.
 *		- ERROR when input parameters are not valid
 *		- an error when input buffer has invalid values
 *
 * @note	None.
 *
 *****************************************************************************/
static u32 XSecure_ConvertStringToHex(char *Str, u32 *Buf, u8 Len)
{
	u32 Status = XST_SUCCESS;
	u8 ConvertedLen = 0;
	u8 Index = 0;
	u8 Nibble[XSECURE_MAX_NIBBLES] = {0U};
	u8 NibbleNum;

	while (ConvertedLen < Len) {
		/* Convert char to nibble */
		for (NibbleNum = 0U;
		NibbleNum < XSECURE_ARRAY_LENGTH(Nibble); NibbleNum++) {
			Status = XSecure_ConvertCharToNibble(
				Str[ConvertedLen], &Nibble[NibbleNum]);
				ConvertedLen = ConvertedLen + 1U;

			if (Status != (u32)XST_SUCCESS) {
				/* Error converting char to nibble */
				Status =  XSECURE_STRING_INVALID_ERROR;
				goto END;
			}
		}

		Buf[Index] = (((u32)Nibble[0] << 28) | ((u32)Nibble[1] << 24) |
				((u32)Nibble[2] << 20) | ((u32)Nibble[3] << 16) |
				((u32)Nibble[4] << 12) | ((u32)Nibble[5] << 8) |
				((u32)Nibble[6] << 4) | ((u32)Nibble[7]));
		Index = Index + 1U;
	}

END:
	return Status;
}


/*****************************************************************************/
/** This is the function to decrypt the encrypted data and load back to memory
 *
 * @param	WrSize: Number of bytes that the encrypted image contains
 *
 * @param       WrAddrHigh: Higher 32-bit Linear memory space from where CSUDMA
 *              will read the data
 *
 * @param       WrAddrLow: Lower 32-bit Linear memory space from where CSUDMA
 *              will read the data
 *
 * @return	None
 *
 *****************************************************************************/
static u32 XSecure_Decrypt(u32 WrSize, u32 SrcAddrHigh, u32 SrcAddrLow)
{
	u32 Status;
	u64 WrAddr;
	u8 Index;
	u32 Size;

	Size = WrSize * XSECURE_WORD_LEN;
	WrAddr = ((u64)SrcAddrHigh << 32) | (u64)SrcAddrLow;
	(void)XSecure_ConvertStringToHex((char *)((char *)(UINTPTR)WrAddr + Size),
					Key, XSECURE_KEY_STR_LEN);
	(void)XSecure_ConvertStringToHex(
		(char *)((char *)(UINTPTR)WrAddr + XSECURE_KEY_STR_LEN + Size),
				Iv, XSECURE_IV_STR_LEN);

	/* Xilsecure expects Key in big endian form */
	for (Index = 0U; Index < XSECURE_ARRAY_LENGTH(Key); Index++) {
		Key[Index] = Xil_Htonl(Key[Index]);
	}
	for (Index = 0U; Index < XSECURE_ARRAY_LENGTH(Iv); Index++) {
		Iv[Index] = Xil_Htonl(Iv[Index]);
	}

	/* Initialize the Aes driver so that it's ready to use */
	(void)XSecure_AesInitialize(&SecureAes, &CsuDma, XSECURE_CSU_AES_KEY_SRC_KUP,
			                           Iv, Key);
	Status = (u32)XSecure_AesDecrypt(&SecureAes,
				(u8 *)(UINTPTR)WrAddr,
			(u8 *)(UINTPTR)WrAddr, Size - (u32)XSECURE_GCM_TAG_LEN);

	return Status;
}

/*****************************************************************************/
/** This is the function to authenticate or decrypt or both for secure images
  * based on flags
 *
 * @param	WrSize: Number of bytes that the secure image contains
 *
 * @param       SrcAddrHigh: Higher 32-bit Linear memory space from where CSUDMA
 *              will read the data
 *
 * @param       SrcAddrLow: Lower 32-bit Linear memory space from where CSUDMA
 *              will read the data
 * @param	flags:
 *		 1 - Decrypt the image.
 *		 0 - Authenticate the image.
 *		 0 - Authenticated and decrypt the image.
 * NOTE -
 *  The current implementation supports only decryption of images
 *
 * @return	error or success based on implementation in secure libraries
 *
 *****************************************************************************/
u32 XSecure_RsaAes(u32 SrcAddrHigh, u32 SrcAddrLow, u32 WrSize, u32 Flags)
{
	u32 Status;

	switch (Flags & XSECURE_MASK) {
	case XSECURE_AES:
		Status = XSecure_CsuDmaInit();
		if (Status != (u32)XST_SUCCESS) {
			Status =  XSECURE_ERROR_CSUDMA_INIT_FAIL;
			goto END;
		}
		Status = XSecure_Decrypt(WrSize, SrcAddrHigh, SrcAddrLow);
		break;
	case XSECURE_RSA:

	case XSECURE_RSA_AES:

	default:
		Status = XSECURE_INVALID_FLAG;
		break;
	}
END:
	return Status;
}

 /****************************************************************************/
 /**
 * This function access the xilsecure SHA3 hardware based on the flags provided
 * to calculate the SHA3 hash.
 *
 * @param	SrcAddrHigh  Higher 32-bit of the input or output address.
 * @param	SrcAddrLow   Lower 32-bit of the input or output address.
 * @param	SrcSize      Size of the data on which hash should be
 *		calculated.
 * @param	Flags: inputs for the operation requested
 *		BIT(0) - for initializing csudma driver and SHA3,
 *		(Here address and size inputs can be NULL)
 *		BIT(1) - To call Sha3_Update API which can be called multiple
 *		times when data is not contiguous.
 *		BIT(2) - to get final hash of the whole updated data.
 *		Hash will be overwritten at provided address with 48 bytes.
 *
 * @return	Returns Status XST_SUCCESS on success and error code on failure.
 *
 *****************************************************************************/
u32 XSecure_Sha3Hash(u32 SrcAddrHigh, u32 SrcAddrLow, u32 SrcSize, u32 Flags)
{
	u32 Status = XST_SUCCESS;
	u64 SrcAddr = ((u64)SrcAddrHigh << 32) | (u64)SrcAddrLow;

	switch (Flags & XSECURE_SHA3_MASK) {
	case XSECURE_SHA3_INIT:
		Status = XSecure_CsuDmaInit();
		if (Status != (u32)XST_SUCCESS) {
			Status = XSECURE_ERROR_CSUDMA_INIT_FAIL;
			goto END;
		}

		Status = (u32)XSecure_Sha3Initialize(&Sha3Instance, &CsuDma);
		if (Status != (u32)XST_SUCCESS) {
			Status = XSECURE_SHA3_INIT_FAIL;
			goto END;
		}
		XSecure_Sha3Start(&Sha3Instance);
		break;
	case XSECURE_SHA3HASH_UPDATE:
		XSecure_Sha3Update(&Sha3Instance, (u8 *)(UINTPTR)SrcAddr,
						SrcSize);
		break;
	case XSECURE_SHA3_FINAL:
		XSecure_Sha3Finish(&Sha3Instance, (u8 *)(UINTPTR)SrcAddr);
		break;
	default:
		Status = XSECURE_INVALID_FLAG;
		break;
	}
END:
	return Status;
}

/*****************************************************************************/
/**
 * This is the function to RSA decrypt or encrypt the provided data and load back
 * to memory
 *
 * @param	SrcAddrHigh	Higher 32-bit Linear memory space from where
 *		CSUDMA will read the data
 * @param	SrcAddrLow	Lower 32-bit Linear memory space from where
 *		CSUDMA will read the data
 * @param	WrSize	Number of bytes to be encrypted or decrypted.
 * @param	Flags:
 *		BIT(0) - Encryption/Decryption
 *			 0 - Rsa Private key Decryption
 *			 1 - Rsa Public key Encryption
 *
 * @return	Returns Status XST_SUCCESS on success and error code on failure.
 *
 * @note	Data to be encrypted/Decrypted + Modulus + Exponent
 *		Modulus and Data should always be key size
 *		Exponent : private key's exponent is key size while decrypting
 *		the signature and 32 bit for public key for encrypting the
 *		signature
 *		In this API we are not taking exponentiation value.
 *
 *****************************************************************************/
u32 XSecure_RsaCore(u32 SrcAddrHigh, u32 SrcAddrLow, u32 SrcSize,
				u32 Flags)
{
	u32 Status;
	u64 WrAddr = ((u64)SrcAddrHigh << 32) | (u64)SrcAddrLow;
	u8 *Modulus = (u8 *)(UINTPTR)(WrAddr + SrcSize);
	u8 *Exponent = (u8 *)(UINTPTR)(WrAddr + SrcSize
						+ SrcSize);

	Status = (u32)XSecure_RsaInitialize(&Secure_Rsa, Modulus, NULL, Exponent);
	if (Status != (u32)XST_SUCCESS) {
		goto END;
	}
	switch (Flags & XSECURE_RSA_CORE_OPERATION) {
	case XSECURE_DEC:
		Status = (u32)XSecure_RsaPrivateDecrypt(&Secure_Rsa,
			(u8 *)(UINTPTR)WrAddr, SrcSize, (u8 *)(UINTPTR)WrAddr);
		break;
	case XSECURE_ENC:
		Status = (u32)XSecure_RsaPublicEncrypt(&Secure_Rsa,
			(u8 *)(UINTPTR)WrAddr, SrcSize, (u8 *)(UINTPTR)WrAddr);
		break;
	default:
		Status = XSECURE_INVALID_FLAG;
		break;
	}

END:
	return Status;
}

/*****************************************************************************/
/*
 * @brief
 * This function initializes the AES-GCM engine with Key and Iv
 *
 * @param	AddrHigh	Higher 32 bit address of the XSecure_AesParams
 * 				structure.
 * @param	AddrLow		Lower 32 bit address of the XSecure_AesParams
 * 				structure.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- Error code on failure
 *
 *****************************************************************************/
static u32 XSecure_InitAes(u32 AddrHigh, u32 AddrLow)
{
	u64 WrAddr = ((u64)AddrHigh << 32) | (u64)AddrLow;
	XSecure_AesParams *AesParams = (XSecure_AesParams *)(UINTPTR)WrAddr;
	u32 Status;
	u8 Index;
	u64 IvAddr;
	u64 KeyAddr;
	u32 *IvPtr;
	u32 *KeyPtr;


	IvAddr = AesParams->Iv;
	KeyAddr = AesParams->Key;

	IvPtr = (u32 *)(UINTPTR)IvAddr;
	KeyPtr =(u32 *)(UINTPTR)KeyAddr;

	Status = XSecure_CsuDmaInit();
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_ERROR_CSUDMA_INIT_FAIL;
		goto END;
	}
	for (Index = 0U; Index < XSECURE_IV_LEN; Index++) {
		Iv[Index] = *IvPtr;
		IvPtr++;
	}
	/* Initialize the Aes driver so that it's ready to use */
	if (AesParams->KeySrc == XSECURE_AES_KUP_KEY) {
		for (Index = 0U; Index < XSECURE_KEY_LEN; Index++) {
			Key[Index] = *KeyPtr;
			KeyPtr++;
		}
		(void)XSecure_AesInitialize(&SecureAes, &CsuDma,
				XSECURE_CSU_AES_KEY_SRC_KUP,
				Iv, Key);
	}
	else {
		(void)XSecure_AesInitialize(&SecureAes, &CsuDma,
				XSECURE_CSU_AES_KEY_SRC_DEV,
				Iv, NULL);
	}
END:
	return Status;

}

/*****************************************************************************/
/**
 * @brief
 * This function performs the AES decryption of the data-blob.
 *
 * @param	AddrHigh	Higher 32 bit address of the XSecure_AesParams
 * 				structure.
 * @param	AddrLow		Lower 32 bit address of the XSecure_AesParams
 * 				structure.
 *
 * @return	Returns Status
 *		- XST_SUCCESS on success
 *		- Error code on failure
 *
 *****************************************************************************/
static u32 XSecure_DecryptData(u32 AddrHigh, u32 AddrLow)
{
	u64 WrAddr = ((u64)AddrHigh << 32) | (u64)AddrLow;
	XSecure_AesParams *AesParams = (XSecure_AesParams *)(UINTPTR)WrAddr;
	u32 Status;
	u32 KeyClearStatus;
	u64 SrcAddr;
	u64 DstAddr;
	u64 GcmTagAddr;

	Status = XSecure_InitAes(AddrHigh,AddrLow);
	if (Status != (u32)XST_SUCCESS) {
		goto END;
	}
	SrcAddr = AesParams->Src;
	DstAddr = AesParams->Dst;

	GcmTagAddr = (u64)(UINTPTR)((u32 *)(UINTPTR)SrcAddr +
			(AesParams->Size/4U));

	Status = (u32)XSecure_AesDecryptData(&SecureAes,
			(u8 *)(UINTPTR)DstAddr,
			(u8 *)(UINTPTR)SrcAddr,
			(u32)AesParams->Size,
			(u8 *)(UINTPTR)GcmTagAddr);

	KeyClearStatus = XSecure_AesKeyZero(&SecureAes);
	if (KeyClearStatus != (u32)XST_SUCCESS) {
		Status = KeyClearStatus;
		goto END;
	}
END:
	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function performs the AES encryption of the data-blob.
 *
 * @param	AddrHigh	Higher 32 bit address of the XSecure_AesParams
 * 				structure.
 * @param	AddrLow		Lower 32 bit address of the XSecure_AesParams
 * 				structure.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- Error code on failure
 *
 *****************************************************************************/
static u32 XSecure_EncryptData(u32 AddrHigh, u32 AddrLow)
{
	u64 WrAddr = ((u64)AddrHigh << 32) | (u64)AddrLow;
	XSecure_AesParams *AesParams = (XSecure_AesParams *)(UINTPTR)WrAddr;
	u32 Status;
	u32 KeyClearStatus;
	u64 SrcAddr;
	u64 DstAddr;

	Status = XSecure_InitAes(AddrHigh,AddrLow);
	if (Status != (u32)XST_SUCCESS) {
		 goto END;
	}
	SrcAddr = AesParams->Src;
	DstAddr = AesParams->Dst;

	XSecure_AesEncryptData(&SecureAes,
			(u8 *)(UINTPTR)DstAddr,
			(u8 *)(UINTPTR)SrcAddr,
			(u32)AesParams->Size);

	KeyClearStatus = XSecure_AesKeyZero(&SecureAes);
        if (KeyClearStatus != (u32)XST_SUCCESS) {
                Status =  KeyClearStatus;
		goto END;
        }
END:
	return Status;
}

/*****************************************************************************/
/*
 * @brief
 * This function checks for flags field of the XSecure_AesParams struct
 * and resolves weather the request is encryption or decryption.
 *
 * @param	AddrHigh	Higher 32 bit address of the XSecure_AesParams
 * 				structure.
 * @param	AddrLow		Lower 32 bit address of the XSecure_AesParams
 * 				structure.
 *
 * @return      Returns Status
 * 		- XST_SUCCESS on success
 * 		- Error code on failure
 *
 ******************************************************************************/
u32 XSecure_AesOperation(u32 AddrHigh, u32 AddrLow)
{
	u64 WrAddr = ((u64)AddrHigh << 32) | (u64)AddrLow;
	XSecure_AesParams *AesParams = (XSecure_AesParams *)(UINTPTR)WrAddr;
	u32 Status;
	u32 Flags = AesParams->AesOp;

#ifndef XSECURE_TRUSTED_ENVIRONMENT
	if (AesParams->KeySrc != XSECURE_AES_KUP_KEY) {
		Status = XSECURE_DEC_WRONG_KEY_SOURCE;
		goto END;
	}
#endif
	if ((AesParams->Size % XSECURE_WORD_LEN) != 0x00U) {
		Status =  XSECURE_SIZE_ERR;
		goto END;
	}
	switch (Flags) {
		case XSECURE_DEC:
			Status = XSecure_DecryptData(AddrHigh, AddrLow);
			break;
		case XSECURE_ENC:
			Status = XSecure_EncryptData(AddrHigh, AddrLow);
			break;
		default:
			Status = XSECURE_INVALID_FLAG;
			break;
	}
END:
	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function performs authentication of data by encrypting the signature
 * with provided key and compares with hash of the data and returns success or
 * failure.
 *
 * @param	Signature	Pointer to the RSA signature of the data
 * @param	Key		Pointer to XSecure_RsaKey structure.
 * @param	Hash		Pointer to the hash of the data to be
 *		authenticated.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- Error code on failure
 *
 *****************************************************************************/
u32 XSecure_DataAuth(u8 *Signature, XSecure_RsaKey *KeyInst, u8 *Hash)
{

	u32 Status;
	XSecure_Rsa SecureRsaInst = {0U};
	u8 EncSignature[XSECURE_MOD_LEN]  = {0U};

	/* Assert conditions */
	Xil_AssertNonvoid(Signature != NULL);
	Xil_AssertNonvoid(KeyInst != NULL);
	Xil_AssertNonvoid((KeyInst->Modulus != NULL) && (KeyInst->Exponent != NULL));
	Xil_AssertNonvoid(Hash != NULL);

	/* Initialize RSA instance */
	Status = (u32)XSecure_RsaInitialize(&SecureRsaInst,
			KeyInst->Modulus, KeyInst->Exponentiation, KeyInst->Exponent);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_RSA_INIT_ERR;
		goto END;
	}

	/* Encrypt signature with RSA key */
	Status = (u32)XSecure_RsaPublicEncrypt(&SecureRsaInst, Signature,
			XSECURE_MOD_LEN, EncSignature);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_RSA_ENCRYPT_ERR;
		goto END;
	}


	/* Compare encrypted signature with sha3 hash calculated on data */
	Status = XSecure_RsaSignVerification(EncSignature, Hash,
					XSECURE_HASH_TYPE_SHA3);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_VERIFY_ERR;
		goto END;
	}

END:
	if (Status != (u32)XST_SUCCESS) {
		Status = Status | (u32)XSECURE_AUTH_FAILURE;
	}

	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function performs authentication of a partition of an image.
 *
 * @param	CsuDmaInstPtr	Pointer to the CSU DMA instance.
 * @param	Data		Pointer to partition to be authenticated.
 * @param	Size		Represents the size of the partition.
 * @param	AuthCertPtr	Pointer to authentication certificate of the
 *		partition.
 *
 * @return	Returns Status
 *		- XST_SUCCESS on success
 *		- Error code on failure
 *
 *****************************************************************************/
u32 XSecure_PartitionAuthentication(XCsuDma *CsuDmaInstPtr, u8 *Data,
				u32 Size, u8 *AuthCertPtr)
{
	u32 Status;
	XSecure_Sha3 SecureSha3 = {0U};
	u8 Hash[XSECURE_HASH_TYPE_SHA3] = {0U};
	u8 *Signature = (AuthCertPtr + XSECURE_AUTH_CERT_PARTSIG_OFFSET);
	XSecure_RsaKey KeyInst;
	u8 *AcPtr = AuthCertPtr;

	/* Assert conditions */
	Xil_AssertNonvoid(CsuDmaInstPtr != NULL);
	Xil_AssertNonvoid(Data != NULL);
	Xil_AssertNonvoid(Size != 0x00U);
	Xil_AssertNonvoid(AuthCertPtr != NULL);

	/* Initialize Sha and RSA instances */
	Status = (u32)XSecure_Sha3Initialize(&SecureSha3, CsuDmaInstPtr);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_SHA3_INIT_FAIL;
		goto END;
	}

	/* Calculate Hash on Data to be authenticated */
	XSecure_Sha3Start(&SecureSha3);
	XSecure_Sha3Update(&SecureSha3, Data, Size);
	XSecure_Sha3Update(&SecureSha3, AuthCertPtr,
		(XSECURE_AUTH_CERT_MIN_SIZE - XSECURE_PARTITION_SIG_SIZE));
	XSecure_Sha3Finish(&SecureSha3, Hash);

	AcPtr += ((u32)XSECURE_RSA_AC_ALIGN + (u32)XSECURE_PPK_SIZE);
	KeyInst.Modulus = AcPtr;

	AcPtr += XSECURE_SPK_MOD_SIZE;
	KeyInst.Exponentiation = AcPtr;

	AcPtr += XSECURE_SPK_MOD_EXT_SIZE;
	KeyInst.Exponent = AcPtr;

	Status = XSecure_DataAuth(Signature, &KeyInst, Hash);
END:
	if (Status != (u32)XST_SUCCESS) {
		Status = Status | (u32)XSECURE_AUTH_FAILURE;
	}

	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function performs authentication of a boot header of the image.
 *
 * @param	CsuDmaInstPtr	Pointer to the CSU DMA instance.
 * @param	Data		pointer to boot header of the image.
 * @param	Size 		Size of the boot header.
 * @param	AuthCertPtr	Pointer to authentication certificate.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- XST_FAILURE on failure
 *
 *****************************************************************************/
static inline u32 XSecure_BhdrAuthentication(XCsuDma *CsuDmaInstPtr,
				u8 *Data, u32 Size, u8 *AuthCertPtr)
{

	u32 Status;
	XSecure_Sha3 SecureSha3 = {0U};
	u8 Hash[XSECURE_HASH_TYPE_SHA3] = {0U};
	u8 *Signature = (AuthCertPtr + XSECURE_AUTH_CERT_BHDRSIG_OFFSET);
	XSecure_RsaKey KeyInst;
	u8 *AcPtr = AuthCertPtr;

	/* Assert conditions */
	Xil_AssertNonvoid(CsuDmaInstPtr != NULL);
	Xil_AssertNonvoid(Data != NULL);
	Xil_AssertNonvoid(Size != 0x00U);
	Xil_AssertNonvoid(AuthCertPtr != NULL);

	/* Initialize Sha and RSA instances */
	Status = (u32)XSecure_Sha3Initialize(&SecureSha3, CsuDmaInstPtr);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_SHA3_INIT_FAIL;
		goto END;
	}

	Status = (u32)XSecure_Sha3PadSelection(&SecureSha3,
					XSECURE_CSU_KECCAK_SHA3);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_SHA3_PADSELECT_ERR;
		goto END;
	}
	/* Calculate Hash on Data to be authenticated */
	XSecure_Sha3Start(&SecureSha3);
	XSecure_Sha3Update(&SecureSha3, Data, Size);
	XSecure_Sha3Finish(&SecureSha3, Hash);

	AcPtr += ((u32)XSECURE_RSA_AC_ALIGN + (u32)XSECURE_PPK_SIZE);
	KeyInst.Modulus = AcPtr;

	AcPtr += XSECURE_SPK_MOD_SIZE;
	KeyInst.Exponentiation = AcPtr;

	AcPtr += XSECURE_SPK_MOD_EXT_SIZE;
	KeyInst.Exponent = AcPtr;


	Status = XSecure_DataAuth(Signature, &KeyInst, Hash);
	if (Status != (u32)XST_SUCCESS) {
		goto END;
	}

END:
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_BOOT_HDR_FAIL | Status;
	}

	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function copies the data from specified source to destination
 * using CSU DMA.
 *
 * @param	DestPtr		Pointer to the destination address.
 * @param	SrcPtr		Pointer to the source address.
 * @param	Size 		Data size to be copied.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- XST_FAILURE on failure
 *
 *****************************************************************************/
u32 XSecure_MemCopy(void * DestPtr, void * SrcPtr, u32 Size)
{

	XSecure_SssSetup(XSecure_SssInputDstDma
			(XSECURE_CSU_SSS_SRC_SRC_DMA));


	/* Data transfer in loop back mode */
	XCsuDma_Transfer(&CsuDma, XCSUDMA_DST_CHANNEL,
				(UINTPTR)DestPtr, Size, 1);
	XCsuDma_Transfer(&CsuDma, XCSUDMA_SRC_CHANNEL,
				(UINTPTR)SrcPtr, Size, 1);

	/* Polling for transfer to be done */
	XCsuDma_WaitForDone(&CsuDma, XCSUDMA_DST_CHANNEL);

	/* To acknowledge the transfer has completed */
	XCsuDma_IntrClear(&CsuDma, XCSUDMA_SRC_CHANNEL, XCSUDMA_IXR_DONE_MASK);
	XCsuDma_IntrClear(&CsuDma, XCSUDMA_DST_CHANNEL, XCSUDMA_IXR_DONE_MASK);

	return XST_SUCCESS;
}

/*****************************************************************************/
/**
 * @brief
 * This function verifies SPK by authenticating SPK with PPK, also checks either
 * SPK is revoked or not.if it is not boot header authentication.
 *
 * @param	AcPtr		Pointer to the authentication certificate.
 * @param	EfuseRsaenable	Input variable which holds
 *		efuse RSA authentication or boot header authentication.
 *		0 - Boot header authentication
 *		1 - RSA authentication
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- Error code on failure
 *
 *****************************************************************************/
u32 XSecure_VerifySpk(u8 *AcPtr, u32 EfuseRsaenable)
{
	u32 Status = XST_FAILURE;
	u32 IsRsaenabled = XSecure_IsRsaEnabled();

	/* Check the cache value with the value passed */
	if (IsRsaenabled != EfuseRsaenable) {
		goto END;
	}
	if (IsRsaenabled != 0x00U) {
		/* Verify SPK with verified PPK */
		Status = XSecure_SpkAuthentication(&CsuDma, AcPtr, EfusePpk);
		if (Status != (u32)XST_SUCCESS) {
			goto END;
		}
		/* SPK revocation */
		Status = XSecure_SpkRevokeCheck(AcPtr);
		if (Status != (u32)XST_SUCCESS) {
			goto END;
		}
	} else {
		/* Verify SPK with PPK in authentication certificate */
		Status = XSecure_SpkAuthentication(&CsuDma, AcPtr, NULL);
		if (Status != (u32)XST_SUCCESS) {
			goto END;
		}
	}

END:

	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function authenticates the single partition image's boot header and
 * image header, also copies all the required details to the ImageInfo pointer.
 *
 * @param	StartAddr	Pointer to the single partition image.
 * @param	ImageInfo	Pointer to XSecure_ImageInfo structure.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- Error code on failure
 *		- XSECURE_AUTH_NOT_ENABLED - represents image is not
 *		authenticated.
 *
 * @note	Copies the header and authentication certificate to internal
 *		buffer.
 *
 *****************************************************************************/
u32 XSecure_AuthenticationHeaders(u8 *StartAddr, XSecure_ImageInfo *ImageInfo)
{
	u32 ImgAttributes;
	u32 Status;
	u32 ImgHdrToffset;
	u32 AuthCertOffset;
	u32 SizeofBH;
	u32 SizeofImgHdr;
	u32 PhOffset;
	u8 *IvPtr = (u8 *)(UINTPTR)Iv;
	u8 BootgenBinFormat[] = {0x66, 0x55, 0x99, 0xAA,
				 0x58, 0x4E, 0x4C, 0x58};/* Sync Word */

	/* validate the  image */
	if((memcmp((StartAddr + XSECURE_IMAGE_SYNC_WORD_OFFSET),
				BootgenBinFormat,
				XSECURE_ARRAY_LENGTH(BootgenBinFormat)))){
		Status = XSECURE_INVALID_IMAGE_ERROR;
		goto END;
	}

	/* Pointer IV available */
	ImageInfo->Iv = Iv;

	(void)memset(Buffer, 0, XSECURE_BUFFER_SIZE);
	(void)memset(AcBuf, 0, XSECURE_AUTH_CERT_MIN_SIZE);

	ImageInfo->EfuseRsaenable = XSecure_IsRsaEnabled();

	/* Copy boot header to internal buffer */
	(void)XSecure_MemCopy(Buffer, StartAddr,
		XSECURE_BOOT_HDR_MAX_SIZE/XSECURE_WORD_LEN);

	/* Know image header's authentication certificate */
	ImgHdrToffset = Xil_In32((UINTPTR)Buffer + XSECURE_IMAGE_HDR_OFFSET);
	AuthCertOffset = Xil_In32((UINTPTR)(StartAddr +
			ImgHdrToffset + XSECURE_AC_IMAGE_HDR_OFFSET)) *
					XSECURE_WORD_LEN;
	if (AuthCertOffset == 0x00U) {
		if (ImageInfo->EfuseRsaenable != 0x00U) {
			Status = XSECURE_AUTH_ISCOMPULSORY;
			goto END;
		}
		Status = XSECURE_AUTH_NOT_ENABLED;
		goto END;
	}

	/* Copy Image header authentication certificate to local memory */
	(void)XSecure_MemCopy(AcBuf, (u8 *)(StartAddr + AuthCertOffset),
			XSECURE_AUTH_CERT_MIN_SIZE/XSECURE_WORD_LEN);

	ImgAttributes = Xil_In32((UINTPTR)(Buffer +
				XSECURE_IMAGE_ATTR_OFFSET));

	if ((ImgAttributes & XSECURE_IMG_ATTR_BHDR_MASK) != 0x00U) {
		ImageInfo->BhdrAuth = XSECURE_ENABLED;
	}

	/* If PUF helper data exists in boot header */
	if ((ImgAttributes & XSECURE_IMG_ATTR_PUFHD_MASK) != 0x00U) {
		SizeofBH = XSECURE_BOOT_HDR_MAX_SIZE;
	}
	else {
		SizeofBH = (u32)XSECURE_BOOT_HDR_MIN_SIZE;
	}

	/* Authenticate keys */
	if (ImageInfo->EfuseRsaenable != 0x00U) {
		if (ImageInfo->BhdrAuth != 0x00U) {
			Status = XSECURE_BHDR_AUTH_NOT_ALLOWED;
			goto END;
		}
	}
	if (ImageInfo->EfuseRsaenable == 0x00U) {
		if (ImageInfo->BhdrAuth == 0x00U) {
			Status = XSECURE_ONLY_BHDR_AUTH_ALLOWED;
			goto END;
		}
	}

	if (ImageInfo->EfuseRsaenable != 0x0U) {
		/* Verify PPK hash with eFUSE */
		Status = XSecure_PpkVerify(&CsuDma, AcBuf);
		if (Status != (u32)XST_SUCCESS) {
			goto END;
		}
		/* Copy PPK to global variable for future use */
		(void)XSecure_MemCopy(EfusePpk, AcBuf + XSECURE_AC_PPK_OFFSET,
				XSECURE_PPK_SIZE/XSECURE_WORD_LEN);
	}
	/* SPK authentication */
	Status = XSecure_SpkAuthentication(&CsuDma, AcBuf, NULL);
	if (Status != (u32)XST_SUCCESS) {
		Status = Status | (u32)XSECURE_BOOT_HDR_FAIL;
		goto END;
	}
	if (ImageInfo->EfuseRsaenable != 0x00U) {
		/* SPK revocation check */
		Status = XSecure_SpkRevokeCheck(AcBuf);
		if (Status != (u32)XST_SUCCESS) {
			Status = Status | (u32)XSECURE_BOOT_HDR_FAIL;
			goto END;
		}
	}
	/* Authenticated boot header */
	Status = XSecure_BhdrAuthentication(&CsuDma, Buffer, SizeofBH, AcBuf);
	if (Status != (u32)XST_SUCCESS) {
		goto END;
	}

	/* Copy boot header parameters */
	ImageInfo->KeySrc =
		Xil_In32((UINTPTR)(Buffer + XSECURE_KEY_SOURCE_OFFSET));
	/* Copy secure header IV */
	if (ImageInfo->KeySrc != 0x00U) {
		(void)XSecure_MemCopy(ImageInfo->Iv, (Buffer + XSECURE_IV_OFFSET),
						XSECURE_IV_SIZE);
	}

	/* Image header Authentication */

	/* Copy image header to internal memory */
	SizeofImgHdr = AuthCertOffset - ImgHdrToffset;
	(void)XSecure_MemCopy(Buffer, (u8 *)(StartAddr + ImgHdrToffset),
					SizeofImgHdr/XSECURE_WORD_LEN);

	/* Authenticate image header */
	Status = XSecure_PartitionAuthentication(&CsuDma,
			Buffer, SizeofImgHdr, (u8 *)(UINTPTR)AcBuf);
	if (Status != (u32)XST_SUCCESS) {
		Status = Status | (u32)XSECURE_IMG_HDR_FAIL;
		goto END;
	}
	 /*
	  * After image header authentication just making sure we
	  * have used proper AC for authenticating
	  */
	if ((Xil_In32((UINTPTR)(Buffer +
		XSECURE_AC_IMAGE_HDR_OFFSET)) * XSECURE_WORD_LEN) !=
						AuthCertOffset) {
		Status = XSECURE_IMG_HDR_FAIL;
		goto END;
	}

	/* Partition header */
	PhOffset = Xil_In32((UINTPTR)(Buffer + XSECURE_PH_OFFSET));

	/* Partition header offset is w.r.t to image start address */
	XSecure_PartitionHeader *Ph = (XSecure_PartitionHeader *)((UINTPTR)
		(Buffer + ((PhOffset * XSECURE_WORD_LEN) - ImgHdrToffset)));

	if (Ph->NextPartitionOffset != 0x00U) {
		Status = XSECURE_IMAGE_WITH_MUL_PARTITIONS;
		goto END;
	}
	ImageInfo->PartitionHdr = Ph;

	/* Add partition header IV to boot header IV */
	if (ImageInfo->KeySrc != 0x00U) {
		*(IvPtr + XSECURE_IV_LEN) = (*(IvPtr + XSECURE_IV_LEN)) +
						(Ph->Iv & XSECURE_PH_IV_MASK);
	}

END:

	return Status;

}

/*****************************************************************************/
/**
 * @brief
 * This function process the secure image of single partition created by
 * bootgen.
 *
 * @param	AddrHigh	Higher 32-bit linear memory space of single
 *		partition non-bitstream image.
 * @param	AddrLow		Lower 32-bit linear memory space of single
 *		partition non-bitstream image.
 * @param	KupAddrHigh	Higher 32-bit linear memory space of KUP key.
 * @param	KupAddrLow	Ligher 32-bit linear memory space of KUP key.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on success
 * 		- Error code on failure
 *
 *****************************************************************************/
u32 XSecure_SecureImage(u32 AddrHigh, u32 AddrLow,
		u32 KupAddrHigh, u32 KupAddrLow, XSecure_DataAddr *Addr)
{
	u8* SrcAddr = (u8 *)(UINTPTR)(((u64)AddrHigh << XSECURE_WORD_SHIFT) |
								AddrLow);
	u32 Status;
	XSecure_ImageInfo ImageHdrInfo = {0};
	u8 *KupKey = (u8 *)(UINTPTR)(((u64)KupAddrHigh << XSECURE_WORD_SHIFT) |
							KupAddrLow);
	u8 NoAuth = 0;
	u32 EncOnly;
	u8 IsEncrypted;
	u8 *IvPtr = (u8 *)(UINTPTR)Iv;
	u8 *EncSrc;
	u8 *DecDst;
	u8 Index;
	u32 Offset;

	/* Address provided is null */
	if (SrcAddr == NULL) {
		Status = (u32)XST_FAILURE;
		goto END;
	}
	/* Initialize CSU DMA driver */
	Status = XSecure_CsuDmaInit();
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_ERROR_CSUDMA_INIT_FAIL;
		goto END;
	}
	/* Headers authentication */
	Status = XSecure_AuthenticationHeaders(SrcAddr, &ImageHdrInfo);
	if (Status != (u32)XST_SUCCESS) {
	/* Error other than XSECURE_AUTH_NOT_ENABLED error will be an error */
		if (Status != XSECURE_AUTH_NOT_ENABLED) {
			goto END;
		}
		else {
			/* Here Buffer still contains Boot header */
			NoAuth = 1;
		}
	}
	else {
		/*
		 * In case of partition is not authenticated and
		 * headers are only authenticated
		 */
		Addr->AddrHigh = (u32)((((u64)(UINTPTR)SrcAddr) +
				(ImageHdrInfo.PartitionHdr->DataWordOffset *
						XSECURE_WORD_LEN)) >> 32);
		Addr->AddrLow = (u32)((UINTPTR)SrcAddr +
					(ImageHdrInfo.PartitionHdr->DataWordOffset *
							XSECURE_WORD_LEN));
	}

	/* If authentication is enabled and authentication of partition is enabled */
	if ((NoAuth == 0U) &&
		((ImageHdrInfo.PartitionHdr->PartitionAttributes &
			XSECURE_PH_ATTR_AUTH_ENABLE) != 0x00U)) {
		/* Copy authentication certificate to internal memory */
		(void)XSecure_MemCopy(AcBuf, (u8 *)(SrcAddr +
			(ImageHdrInfo.PartitionHdr->AuthCertificateOffset *
					XSECURE_WORD_LEN)),
			XSECURE_AUTH_CERT_MIN_SIZE/XSECURE_WORD_LEN);

		Status = XSecure_VerifySpk(AcBuf, ImageHdrInfo.EfuseRsaenable);
		if (Status != (u32)XST_SUCCESS) {
			Status = XSECURE_PARTITION_FAIL | Status;
			goto END;
		}
		Offset = ImageHdrInfo.PartitionHdr->DataWordOffset * XSECURE_WORD_LEN;
		/* Authenticate Partition */
		Status = XSecure_PartitionAuthentication(&CsuDma,
				(u8 *)(SrcAddr + Offset),
			(u32)((ImageHdrInfo.PartitionHdr->TotalDataWordLength *
					XSECURE_WORD_LEN) -
			XSECURE_AUTH_CERT_MIN_SIZE),
			(u8 *)((UINTPTR)AcBuf));
		if (Status != (u32)XST_SUCCESS) {
			Status = Status | (u32)XSECURE_PARTITION_FAIL;
			goto END;
		}
	}

	if (NoAuth != 0x0U) {
		XSecure_PartitionHeader *Ph =
			(XSecure_PartitionHeader *)((UINTPTR)
			(SrcAddr + Xil_In32((UINTPTR)Buffer +
					XSECURE_PH_TABLE_OFFSET)));
		ImageHdrInfo.PartitionHdr = Ph;
		if ((ImageHdrInfo.PartitionHdr->PartitionAttributes &
				XSECURE_PH_ATTR_AUTH_ENABLE) != 0x00U) {
			Status = XSECURE_HDR_NOAUTH_PART_AUTH;
			goto END;
		}
	}

	/* Decrypt the partition if encryption is enabled */
	EncOnly = XSecure_IsEncOnlyEnabled();
	IsEncrypted = (u8)(ImageHdrInfo.PartitionHdr->PartitionAttributes &
						XSECURE_PH_ATTR_ENC_ENABLE);
	if (IsEncrypted != 0x00U) {
		/* key selection */
		if (NoAuth != 0x00U) {
			ImageHdrInfo.KeySrc = Xil_In32((UINTPTR)Buffer +
						XSECURE_KEY_SOURCE_OFFSET);
#ifndef XSECURE_TRUSTED_ENVIRONMENT
			if (ImageHdrInfo.KeySrc != XSECURE_KEY_SRC_KUP) {
				Status = XSECURE_DEC_WRONG_KEY_SOURCE;
				goto END;
			}
#endif
			(void)XSecure_MemCopy(ImageHdrInfo.Iv,
				(Buffer + XSECURE_IV_OFFSET), XSECURE_IV_SIZE);
			/* Add partition header IV to boot header IV */
			*(IvPtr + XSECURE_IV_LEN) =
				(*(IvPtr + XSECURE_IV_LEN)) +
				(ImageHdrInfo.PartitionHdr->Iv & XSECURE_PH_IV_MASK);
		}
		/*
		 * When authentication exists and requesting
		 * for device key other than eFUSE and KUP key
		 * when ENC_ONLY bit is blown
		 */
		if (EncOnly != 0x00U) {
			if ((ImageHdrInfo.KeySrc == XSECURE_KEY_SRC_BBRAM) ||
			(ImageHdrInfo.KeySrc == XSECURE_KEY_SRC_GREY_BH) ||
			(ImageHdrInfo.KeySrc == XSECURE_KEY_SRC_BLACK_BH)) {
				Status = XSECURE_DEC_WRONG_KEY_SOURCE;
				goto END;
			}
		}
	}
	else {
		/* when image is not encrypted */
		if (EncOnly != 0x00U) {
			Status = XSECURE_ENC_ISCOMPULSORY;
			goto END;
		}
		if (NoAuth != 0x00U) {
			Status = XSECURE_ISNOT_SECURE_IMAGE;
			goto END;
		}
		else {
			Status = (u32)XST_SUCCESS;
			goto END;
		}
	}
	if (ImageHdrInfo.KeySrc == XSECURE_KEY_SRC_KUP) {
		if (KupKey != 0x00) {
			/* Linux or U-boot stores Key in the form of String
			 * So this conversion is required here.
			 */
			(void)XSecure_ConvertStringToHex((char *)(UINTPTR)KupKey,
					Key, XSECURE_KEY_STR_LEN);
			/* XilSecure expects Key in big endian form */
			for (Index = 0U; Index < XSECURE_ARRAY_LENGTH(Key);
							Index++) {
				Key[Index] = Xil_Htonl(Key[Index]);
			}
		}
		else {
			Status = XSECURE_KUP_KEY_NOT_PROVIDED;
			goto END;
		}
	 }
	else {
		if (KupKey != 0x00) {
			Status = XSECURE_KUP_KEY_NOT_REQUIRED;
			goto END;
		}
	}

	/* Initialize the AES driver so that it's ready to use */
	if (ImageHdrInfo.KeySrc == XSECURE_KEY_SRC_KUP) {
		(void)XSecure_AesInitialize(&SecureAes, &CsuDma,
				XSECURE_CSU_AES_KEY_SRC_KUP,
				(u32 *)ImageHdrInfo.Iv, Key);
	}
	else {
		(void)XSecure_AesInitialize(&SecureAes, &CsuDma,
					XSECURE_CSU_AES_KEY_SRC_DEV,
					(u32 *)ImageHdrInfo.Iv, NULL);
	}
	if (ImageHdrInfo.PartitionHdr->DestinationLoadAddress == 0x00U) {
		EncSrc = (u8 *)(UINTPTR)(SrcAddr +
				(ImageHdrInfo.PartitionHdr->DataWordOffset *
					XSECURE_WORD_LEN));
		DecDst = (u8 *)(UINTPTR)(SrcAddr +
				(ImageHdrInfo.PartitionHdr->DataWordOffset *
					XSECURE_WORD_LEN));
	}
	else {
		EncSrc = (u8 *)(UINTPTR)(SrcAddr +
				(ImageHdrInfo.PartitionHdr->DataWordOffset *
					XSECURE_WORD_LEN));
		DecDst = (u8 *)(UINTPTR)
			(ImageHdrInfo.PartitionHdr->DestinationLoadAddress);
	}
	Status = (u32)XSecure_AesDecrypt(&SecureAes,
				DecDst, EncSrc,
		(ImageHdrInfo.PartitionHdr->UnEncryptedDataWordLength *
				XSECURE_WORD_LEN));

	if (Status != (u32)XST_SUCCESS) {
		if (Status == (u32)XSECURE_CSU_AES_GCM_TAG_MISMATCH) {
			Status = XSECURE_AES_GCM_TAG_NOT_MATCH;
		}
		else if (Status == (u32)XSECURE_CSU_AES_ZEROIZATION_ERROR) {
			Status = XSECURE_AES_ZEROIZATION_ERR;
		}
		else
		{
			Status = XSECURE_CSU_AES_DEVICE_COPY_ERROR;
		}
		Status = XSECURE_PARTITION_FAIL |
				XSECURE_AES_DECRYPTION_FAILURE | Status;
		Addr->AddrHigh = 0x00U;
		Addr->AddrLow = 0x00U;
	}
	else {
		Addr->AddrHigh = (u32)((u64)(UINTPTR)(DecDst) >> 32);
		Addr->AddrLow = (u32)(UINTPTR)(DecDst);
	}

END:
	if (KupKey != 0x00) {
		/* Clear the user key */
		(void)memset(KupKey, 0, XSECURE_KEY_STR_LEN);
	}
	/* Clear internal buffers */
	(void)memset(Buffer, 0, XSECURE_BUFFER_SIZE);
	(void)memset(AcBuf, 0, XSECURE_AUTH_CERT_MIN_SIZE);
	(void)memset(EfusePpk, 0, XSECURE_PPK_SIZE);

	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function tells whether RSA authentication is enabled or not.
 *
 * @return	Returns Status
 * 		- XSECURE_ENABLED if RSA bit of efuse is programmed
 * 		- XSECURE_NOTENABLED if RSA bit of efuse is not programmed.
 *
 *****************************************************************************/
u32 XSecure_IsRsaEnabled(void)
{
	u32 Status;

	if ((Xil_In32(XSECURE_EFUSE_SEC_CTRL) &
			XSECURE_EFUSE_SEC_CTRL_RSA_ENABLE) != 0x00U) {

		Status = XSECURE_ENABLED;
		goto END;
	}
	Status = XSECURE_NOTENABLED;
END:
	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function tells whether encrypt only is enabled or not.
 *
 * @return	Returns Status
 * 		- XSECURE_ENABLED if enc_only bit of efuse is programmed
 * 		- XSECURE_NOTENABLED if enc_only bit of efuse is not programmed
 *
 *****************************************************************************/
u32 XSecure_IsEncOnlyEnabled(void)
{
	u32 Status;

	if ((Xil_In32(XSECURE_EFUSE_SEC_CTRL) &
			XSECURE_EFUSE_SEC_CTRL_ENC_ONLY) != 0x00U) {

		Status = XSECURE_ENABLED;
		goto END;
	}
	Status = XSECURE_NOTENABLED;
END:
	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function verifies the PPK hash with PPK programmed on efuse.
 *
 * @param	CsuDmaInstPtr	Pointer to CSU DMA instance.
 * @param	AuthCert	Pointer to authentication certificate.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on successful verification.
 * 		- Error code on failure.
 *
 *****************************************************************************/
u32 XSecure_PpkVerify(XCsuDma *CsuDmaInstPtr, u8 *AuthCert)
{
	u8 PpkSel = (*(u32 *)AuthCert & XSECURE_AH_ATTR_PPK_SEL_MASK) >>
					XSECURE_AH_ATTR_PPK_SEL_SHIFT;
	u32 Status;
	u32 Hash[XSECURE_HASH_TYPE_SHA3/XSECURE_WORD_LEN] = {0U};
	XSecure_Sha3 Sha3Inst = {0U};
	u8 Index;
	u32 EfusePpkAddr;

	/* Check if PPK selection is correct */
	if (PpkSel > 1U) {
		Status = XSECURE_SEL_ERR;
		goto END;
	}

	/* Calculate PPK hash */
	(void)XSecure_Sha3Initialize(&Sha3Inst, CsuDmaInstPtr);
	Status = (u32)XSecure_Sha3PadSelection(&Sha3Inst,
				XSECURE_CSU_KECCAK_SHA3);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_SHA3_PADSELECT_ERR;
		goto END;
	}
	XSecure_Sha3Digest(&Sha3Inst, AuthCert + XSECURE_AC_PPK_OFFSET,
					XSECURE_KEY_SIZE, (u8 *)Hash);

	/* Check selected is PPK revoked */
	if (PpkSel == 0x0U) {
		EfusePpkAddr = XSECURE_EFUSE_PPK0;
		if ((Xil_In32(XSECURE_EFUSE_SEC_CTRL) &
				XSECURE_EFUSE_SEC_CTRL_PPK0_REVOKE) != 0x00U) {
			Status = XSECURE_REVOKE_ERR;
			goto END;
		}
	}
	else {
		EfusePpkAddr = XSECURE_EFUSE_PPK1;
		if ((Xil_In32(XSECURE_EFUSE_SEC_CTRL) &
				XSECURE_EFUSE_SEC_CTRL_PPK1_REVOKE) != 0x00U) {
			Status = XSECURE_REVOKE_ERR;
			goto END;
		}
	}
	/* Verify PPK hash */
	for (Index = 0U;
		Index < (XSECURE_HASH_TYPE_SHA3 / XSECURE_WORD_LEN); Index++) {
		if (Hash[Index] != Xil_In32(
				EfusePpkAddr + ((u32)Index * (u32)XSECURE_WORD_LEN))) {
			Status = XSECURE_VERIFY_ERR;
			goto END;
		}
	}

END:
	if (Status != (u32)XST_SUCCESS) {
		Status = Status | (u32)XSECURE_PPK_ERR;
	}

	return Status;
}

/*****************************************************************************/
/**
 * @brief
 * This function authenticates SPK with provided PPK or PPK from authentication
 * certificate.
 *
 * @param	CsuDmaInstPtr	Pointer to CSU DMA instance.
 * @param	AuthCert	Pointer to authentication certificate.
 * @param	Ppk		Pointer to the PPK key.
 *		- If NULL uses PPK from provided authentication certificate.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on successful verification.
 * 		- Error code on failure.
 *
 *****************************************************************************/
u32 XSecure_SpkAuthentication(XCsuDma *CsuDmaInstPtr, u8 *AuthCert, u8 *Ppk)
{
	u8 SpkHash[XSECURE_HASH_TYPE_SHA3] = {0U};
	u8 SpkIdFuseSel = (*(u32 *)AuthCert & XSECURE_AH_ATTR_SPK_ID_FUSE_SEL_MASK) >>
                                        XSECURE_AH_ATTR_SPKID_FUSESEL_SHIFT;

	u8* PpkModular;
	u8* PpkModularEx;
	u8* PpkExpPtr;
	u32 Status;
	u8 RsaSha3Array[512] = {0};
	u8 *PpkPtr;
	u8 *AcPtr = (u8 *)AuthCert;

	if (Ppk == NULL) {
		PpkPtr = (AcPtr + XSECURE_RSA_AC_ALIGN);
	}
	else {
		PpkPtr = Ppk;
	}

	/* Initialize sha3 */
	(void)XSecure_Sha3Initialize(&Sha3Instance, CsuDmaInstPtr);
	if (SpkIdFuseSel == XSECURE_SPKID_EFUSE) {
                (void)XSecure_Sha3PadSelection(&Sha3Instance, XSECURE_CSU_KECCAK_SHA3);
        }
        else if (SpkIdFuseSel != XSECURE_USER_EFUSE) {
		Status = XSECURE_INVALID_EFUSE_SELECT;
		goto END;
	}
	else {
		Status = (u32)XST_SUCCESS;
	}

	XSecure_Sha3Start(&Sha3Instance);


	/* Hash the PPK + SPK choice */
	XSecure_Sha3Update(&Sha3Instance, AcPtr, XSECURE_AUTH_HEADER_SIZE);

	/* Set PPK pointer */
	PpkModular = (u8 *)PpkPtr;
	PpkPtr += XSECURE_PPK_MOD_SIZE;
	PpkModularEx = PpkPtr;
	PpkPtr += XSECURE_PPK_MOD_EXT_SIZE;
	PpkExpPtr = PpkPtr;
	AcPtr += ((u32)XSECURE_RSA_AC_ALIGN + (u32)XSECURE_PPK_SIZE);

	/* Calculate SPK + Auth header Hash */
	XSecure_Sha3Update(&Sha3Instance, (u8 *)AcPtr, XSECURE_SPK_SIZE);

	XSecure_Sha3Finish(&Sha3Instance, (u8 *)SpkHash);

	/* Set SPK Signature pointer */
	AcPtr += XSECURE_SPK_SIZE;


	Status = (u32)XSecure_RsaInitialize(&Secure_Rsa, PpkModular,
			PpkModularEx, PpkExpPtr);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_RSA_INIT_ERR;
		goto END;
	}

	/* Decrypt SPK Signature */
	Status = (u32)XSecure_RsaPublicEncrypt(&Secure_Rsa, AcPtr,
				XSECURE_SPK_SIG_SIZE, RsaSha3Array);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_RSA_ENCRYPT_ERR;
		goto END;
	}

	/* Authenticate SPK Signature */
	Status = XSecure_RsaSignVerification(RsaSha3Array,
				SpkHash, XSECURE_HASH_TYPE_SHA3);
	if (Status != (u32)XST_SUCCESS) {
		Status = XSECURE_VERIFY_ERR;
		goto END;
	}


END:
	if (Status != (u32)XST_SUCCESS) {
		Status = Status | (u32)XSECURE_SPK_ERR;
	}

	return Status;

}

/*****************************************************************************/
/**
 * @brief
 * This function checks whether SPK is been revoked or not.
 *
 * @param	AuthCert	Pointer to authentication certificate.
 *
 * @return	Returns Status
 * 		- XST_SUCCESS on successful verification.
 * 		- Error code on failure.
 *
 *****************************************************************************/
u32 XSecure_SpkRevokeCheck(u8 *AuthCert)
{
	u32 Status = XST_SUCCESS;
	u32 *SpkID = (u32 *)(UINTPTR)(AuthCert + XSECURE_AC_SPKID_OFFSET);
	u8 SpkIdFuseSel = (*(u32 *)AuthCert & XSECURE_AH_ATTR_SPK_ID_FUSE_SEL_MASK)
							>>	XSECURE_AH_ATTR_SPKID_FUSESEL_SHIFT;
	u32 UserFuseAddr;
	u32 UserFuseVal;

	/* If SPKID Efuse is selected , Verifies SPKID with Efuse SPKID*/
	if (SpkIdFuseSel == XSECURE_SPKID_EFUSE) {
		if (*SpkID != Xil_In32(XSECURE_EFUSE_SPKID)) {
			Status = (u32)XSECURE_SPK_ERR | (u32)XSECURE_REVOKE_ERR;
		}
	}
	/*
	 *	If User EFUSE is selected, checks the corresponding User-Efuse bit
	 *	programmed or not. If Programmed (indicates that key is revocated)
	 *	throws an error
	 */
	else if (SpkIdFuseSel == XSECURE_USER_EFUSE) {
		if ((*SpkID >= XSECURE_USER_EFUSE_MIN_VALUE) &&
			(*SpkID <= XSECURE_USER_EFUSE_MAX_VALUE)) {
			UserFuseAddr = XSECURE_USER_EFUSE_START_ADDR +
						(((*SpkID-1)/XSECURE_WORD_SHIFT)*XSECURE_WORD_LEN);
			UserFuseVal = Xil_In32(UserFuseAddr);
			if ((UserFuseVal & (0x1U << ((*SpkID - 1) %
								XSECURE_WORD_SHIFT))) != 0x0U) {
				Status = (u32)XSECURE_SPK_ERR | (u32)XSECURE_REVOKE_ERR;
			}
		}
		/*
		 *	Maximum 256 keys can be revocated, if exceeds throws
		 *	out of range error
		 */
		else {
			Status = XSECURE_OUT_OF_RANGE_USER_EFUSE_ERROR;
		}
	}
	else {
		Status = XSECURE_INVALID_EFUSE_SELECT;
	}

	return Status;
}
