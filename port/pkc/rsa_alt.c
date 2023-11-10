/*--------------------------------------------------------------------------*/
/* Copyright 2021 NXP                                                       */
/*                                                                          */
/* NXP Confidential. This software is owned or controlled by NXP and may    */
/* only be used strictly in accordance with the applicable license terms.   */
/* By expressly accepting such terms or by downloading, installing,         */
/* activating and/or otherwise using the software, you are agreeing that    */
/* you have read, and that you agree to comply with and are bound by, such  */
/* license terms. If you do not agree to be bound by the applicable license */
/* terms, then you may not retain, install, activate or otherwise use the   */
/* software.                                                                */
/*--------------------------------------------------------------------------*/

/** @file  rsa_alt.c
 *  @brief alternative RSA implementation with CSS and PKC IPs
 */

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#include <stdint.h>
#include <mcuxClSession.h>          // Interface to the entire mcuxClSession component
#include <mcuxCsslFlowProtection.h> // Code flow protection
#include <mcuxClPkc.h>              // Interface to the entire mcuxClPkc component
#include <mcuxClRsa.h>              // Interface to the entire mcuxClRsa component
#include <mcuxClMemory.h>
#include <mbedtls/error.h>
#include <mbedtls/platform.h>
#include <platform_hw_ip.h>
#include <mbedtls/rsa.h>
#include <rsa_alt.h>
#include "mbedtls/platform_util.h"

#if !defined(MBEDTLS_RSA_CTX_ALT) || !defined(MBEDTLS_RSA_PUBLIC_ALT) || !defined(MBEDTLS_RSA_PRIVATE_ALT)
#error This implmenetation requires that all 3 alternative implementation options are enabled together.
#else

/* Parameter validation macros */
#define RSA_VALIDATE_RET( cond )                                       \
    MBEDTLS_INTERNAL_VALIDATE_RET( cond, MBEDTLS_ERR_RSA_BAD_INPUT_DATA )
#define RSA_VALIDATE( cond )                                           \
    MBEDTLS_INTERNAL_VALIDATE( cond )

/*
 * Do an RSA public key operation
 */
int mbedtls_rsa_public( mbedtls_rsa_context *ctx,
                const unsigned char *input,
                unsigned char *output )
{
    RSA_VALIDATE_RET( ctx != NULL );
    RSA_VALIDATE_RET( input != NULL );
    RSA_VALIDATE_RET( output != NULL );

    if( rsa_check_context( ctx, 0 /* public */, 0 /* no blinding */ ) )
    {
        return( MBEDTLS_ERR_RSA_BAD_INPUT_DATA );
    }

    /**************************************************************************/
    /* Preparation                                                            */
    /**************************************************************************/

    /* Initialize Hardware */
    int ret_hw_init = mbedtls_hw_init();
    if(0!=ret_hw_init)
    {
        return MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED;
    }

    /* Create session handle to be used by verify function */

    /* Get the byte-length of modulus n */
    const uint32_t nByteLength = ctx->len;

    /* CPU buffer */
    uint32_t cpuWaBuffer[MCUXCLRSA_VERIFY_OPTIONNOVERIFY_WACPU_SIZE / sizeof(uint32_t)];

    /* PKC buffer and size */
    uint8_t *pPkcRam = (uint8_t *) MCUXCLPKC_RAM_START_ADDRESS;
    const uint32_t pkcWaSize = MCUXCLPKC_ROUNDUP_SIZE(nByteLength /* modulus */
                                                   + nByteLength /* exponent */
                                                   + nByteLength /* result buffer */);

    mcuxClSession_Descriptor_t sessionDesc;
    mcuxClSession_Handle_t session = &sessionDesc;

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(si_status, si_token, mcuxClSession_init(
                /* mcuxClSession_Handle_t session:      */ session,
                /* uint32_t * const cpuWaBuffer:       */ cpuWaBuffer,
                /* uint32_t cpuWaSize:                 */ MCUXCLRSA_VERIFY_OPTIONNOVERIFY_WACPU_SIZE / sizeof(uint32_t),
                /* uint32_t * const pkcWaBuffer:       */ (uint32_t *) pPkcRam,
                /* uint32_t pkcWaSize:                 */ (pkcWaSize + MCUXCLRSA_VERIFY_OPTIONNOVERIFY_WAPKC_SIZE(nByteLength * 8u)) / sizeof(uint32_t)
                ));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClSession_init) != si_token) || (MCUXCLSESSION_STATUS_OK != si_status))
    {
        return MBEDTLS_ERR_RSA_PUBLIC_FAILED;
    }

    /* Set pointers in PKC */
    uint8_t *pMod = pPkcRam;
    uint8_t *pExp = pMod + nByteLength;
    uint8_t *pBuf = pExp + nByteLength;

    /* Create key struct of type MCUXCLRSA_KEY_PUBLIC */

    /* Get actual parameter lengths */
    size_t modByteLength = (mbedtls_mpi_bitlen(&ctx->N) + 7u) / 8u;
    size_t expByteLength = (mbedtls_mpi_bitlen(&ctx->E) + 7u) / 8u;

    /* Check actual length with length given in the context. */
    if( nByteLength != modByteLength )
    {
        return( MBEDTLS_ERR_RSA_BAD_INPUT_DATA );
    }

    /* Use mbedtls function to extract key parameters in big-endian order */
    mbedtls_mpi_write_binary(&ctx->N, pMod, modByteLength);
    mbedtls_mpi_write_binary(&ctx->E, pExp, expByteLength);

    const mcuxClRsa_KeyEntry_t kMod = {
                       .pKeyEntryData = (uint8_t *) pMod,
                       .keyEntryLength = (uint32_t) modByteLength };

    const mcuxClRsa_KeyEntry_t kExp = {
                       .pKeyEntryData = (uint8_t *) pExp,
                       .keyEntryLength = (uint32_t) expByteLength };

    const mcuxClRsa_Key public_key = {
                                     .keytype = MCUXCLRSA_KEY_PUBLIC,
                                     .pMod1 = (mcuxClRsa_KeyEntry_t *)&kMod,
                                     .pMod2 = NULL,
                                     .pQInv = NULL,
                                     .pExp1 = (mcuxClRsa_KeyEntry_t *)&kExp,
                                     .pExp2 = NULL,
                                     .pExp3 = NULL };

    ctx->rsa_key = public_key;

    session->pkcWa.used += pkcWaSize / sizeof(uint32_t);

    /**************************************************************************/
    /* RSA verify call                                                        */
    /**************************************************************************/

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(verify_result, verify_token, mcuxClRsa_verify(
                /* mcuxClSession_Handle_t           pSession: */           session,
                /* const mcuxClRsa_Key * const      pKey: */               &public_key,
                /* const uint8_t * const           pMessageOrDigest: */   NULL,
                /* const uint32_t                  messageLength: */      0u,
                /* uint8_t * const                 pSignature: */         (uint8_t *)input,
                /* const mcuxClRsa_SignVerifyMode   pVerifyMode: */        (mcuxClRsa_SignVerifyMode_t *)&mcuxClRsa_Mode_Verify_NoVerify,
                /* const uint32_t                  saltLength: */         0u,
                /* uint32_t                        options: */            0u,
                /* uint8_t * const                 pOutput: */            (uint8_t *)pBuf));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClRsa_verify) != verify_token) || (MCUXCLRSA_STATUS_VERIFYPRIMITIVE_OK != verify_result))
    {
        return MBEDTLS_ERR_RSA_PUBLIC_FAILED;
    }

    session->pkcWa.used -= pkcWaSize / sizeof(uint32_t);

    /* Copy result buffer to output */
    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(retMemCpy, tokenMemCpy,
            mcuxClMemory_copy((uint8_t *) output, pBuf, nByteLength, nByteLength) );

    if ((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClMemory_copy) != tokenMemCpy) && (0u != retMemCpy) )
    {
        return MBEDTLS_ERR_RSA_PUBLIC_FAILED;
    }

    /**************************************************************************/
    /* Session clean-up                                                       */
    /**************************************************************************/

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(cleanup_result, cleanup_token, mcuxClSession_cleanup(
                /* mcuxClSession_Handle_t           pSession: */           session));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClSession_cleanup) != cleanup_token) || (MCUXCLSESSION_STATUS_OK != cleanup_result))
    {
        return MBEDTLS_ERR_RSA_PUBLIC_FAILED;
    }

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(destroy_result, destroy_token, mcuxClSession_destroy(
                /* mcuxClSession_Handle_t           pSession: */           session));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClSession_destroy) != destroy_token) || (MCUXCLSESSION_STATUS_OK != destroy_result))
    {
        return MBEDTLS_ERR_RSA_PUBLIC_FAILED;
    }

    return( 0 );
}

/*
 * Do an RSA private key operation
 */
int mbedtls_rsa_private( mbedtls_rsa_context *ctx,
                 int (*f_rng)(void *, unsigned char *, size_t),
                 void *p_rng,
                 const unsigned char *input,
                 unsigned char *output )
{
    RSA_VALIDATE_RET( ctx != NULL );
    RSA_VALIDATE_RET( input != NULL );
    RSA_VALIDATE_RET( output != NULL );

    if( rsa_check_context( ctx, 1 /* private */, 1 /* blinding */ ) != 0 )
    {
        return( MBEDTLS_ERR_RSA_BAD_INPUT_DATA );
    }

    /**************************************************************************/
    /* Preparation                                                            */
    /**************************************************************************/

    /* Initialize Hardware */
    int ret_hw_init = mbedtls_hw_init();
    if(0!=ret_hw_init)
    {
        return MBEDTLS_ERR_PLATFORM_HW_ACCEL_FAILED;
    }

    /* Create session handle to be used by sign function */

    /* Get the byte-length of modulus n */
    const uint32_t nByteLength = ctx->len;
    const uint32_t pqByteLength = (nByteLength+1) / 2u;

    /* CPU buffer */
    uint32_t cpuWaBuffer[MCUXCLRSA_SIGN_CRT_OPTIONNOENCODE_2048_WACPU_SIZE / sizeof(uint32_t)];

    /* PKC buffer and size */
    uint8_t *pPkcRam = (uint8_t *) MCUXCLPKC_RAM_START_ADDRESS;
    const uint32_t pkcWaSize = MCUXCLPKC_ROUNDUP_SIZE((2u * pqByteLength) /* p and q (2 * 1/2 * nByteLength) */
                                                   + (pqByteLength)      /* q_inv */
                                                   + (2u * pqByteLength) /* dp and dq (2 * 1/2 * nByteLength) */
                                                   + (nByteLength)      /* e */
                                                   + (nByteLength)      /* result buffer */);

    mcuxClSession_Descriptor_t sessionDesc;
    mcuxClSession_Handle_t session = &sessionDesc;

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(si_status, si_token, mcuxClSession_init(
                /* mcuxClSession_Handle_t session:      */ session,
                /* uint32_t * const cpuWaBuffer:       */ cpuWaBuffer,
                /* uint32_t cpuWaSize:                 */ MCUXCLRSA_SIGN_CRT_OPTIONNOENCODE_WACPU_SIZE(nByteLength * 8u) / sizeof(uint32_t),
                /* uint32_t * const pkcWaBuffer:       */ (uint32_t *) pPkcRam,
                /* uint32_t pkcWaSize:                 */ (pkcWaSize + MCUXCLRSA_SIGN_CRT_OPTIONNOENCODE_WAPKC_SIZE(nByteLength * 8u)) / sizeof(uint32_t)
                ));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClSession_init) != si_token) || (MCUXCLSESSION_STATUS_OK != si_status))
    {
        return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
    }

    /* Set pointers in PKC */
    uint8_t *pP = pPkcRam;
    uint8_t *pQ = pP + pqByteLength;
    uint8_t *pQ_inv = pQ + pqByteLength;
    uint8_t *pDP = pQ_inv + pqByteLength;
    uint8_t *pDQ = pDP + pqByteLength;
    uint8_t *pE = pDQ + pqByteLength;
    uint8_t *pBuf = pE + nByteLength;

    /* Create key struct of type MCUXCLRSA_KEY_PRIVATECRT */

    /* Get actual parameter lengths */
    size_t pByteLength     = (mbedtls_mpi_bitlen(&ctx->P) + 7u) / 8u;
    size_t qByteLength     = (mbedtls_mpi_bitlen(&ctx->Q) + 7u) / 8u;
    size_t q_invByteLength = (mbedtls_mpi_bitlen(&ctx->QP) + 7u) / 8u;
    size_t dpByteLength    = (mbedtls_mpi_bitlen(&ctx->DP) + 7u) / 8u;
    size_t dqByteLength    = (mbedtls_mpi_bitlen(&ctx->DQ) + 7u) / 8u;
    size_t eByteLength     = (mbedtls_mpi_bitlen(&ctx->E) + 7u) / 8u;

    /* Check actual length with length given in the context. */
    if( (pqByteLength != pByteLength) || (pqByteLength != qByteLength) )
    {
        return( MBEDTLS_ERR_RSA_BAD_INPUT_DATA );
    }

    /* Use mbedtls function to extract key parameters in big-endian order */
    mbedtls_mpi_write_binary(&ctx->P, pP, pByteLength);
    mbedtls_mpi_write_binary(&ctx->Q, pQ, qByteLength);
    mbedtls_mpi_write_binary(&ctx->QP, pQ_inv, q_invByteLength);
    mbedtls_mpi_write_binary(&ctx->DP, pDP, dpByteLength);
    mbedtls_mpi_write_binary(&ctx->DQ, pDQ, dqByteLength);
    mbedtls_mpi_write_binary(&ctx->E, pE, eByteLength);

    const mcuxClRsa_KeyEntry_t kP = {
                       .pKeyEntryData = (uint8_t *) pP,
                       .keyEntryLength = (uint32_t) pByteLength };

    const mcuxClRsa_KeyEntry_t kQ = {
                       .pKeyEntryData = (uint8_t *) pQ,
                       .keyEntryLength = (uint32_t) qByteLength };

    const mcuxClRsa_KeyEntry_t kQ_inv = {
                       .pKeyEntryData = (uint8_t *) pQ_inv,
                       .keyEntryLength = (uint32_t) q_invByteLength };

    const mcuxClRsa_KeyEntry_t kDP = {
                       .pKeyEntryData = (uint8_t *) pDP,
                       .keyEntryLength = (uint32_t) dpByteLength };

    const mcuxClRsa_KeyEntry_t kDQ = {
                       .pKeyEntryData = (uint8_t *) pDQ,
                       .keyEntryLength = (uint32_t) dqByteLength };

    const mcuxClRsa_KeyEntry_t kE = {
                       .pKeyEntryData = (uint8_t *) pE,
                       .keyEntryLength = (uint32_t) eByteLength };

    const mcuxClRsa_Key private_key = {
                                     .keytype = MCUXCLRSA_KEY_PRIVATECRT,
                                     .pMod1 = (mcuxClRsa_KeyEntry_t *)&kP,
                                     .pMod2 = (mcuxClRsa_KeyEntry_t *)&kQ,
                                     .pQInv = (mcuxClRsa_KeyEntry_t *)&kQ_inv,
                                     .pExp1 = (mcuxClRsa_KeyEntry_t *)&kDP,
                                     .pExp2 = (mcuxClRsa_KeyEntry_t *)&kDQ,
                                     .pExp3 = (mcuxClRsa_KeyEntry_t *)&kE };

    ctx->rsa_key = private_key;

    session->pkcWa.used += pkcWaSize / sizeof(uint32_t);

    /**************************************************************************/
    /* RSA sign call                                                          */
    /**************************************************************************/

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(sign_result, sign_token, mcuxClRsa_sign(
                /*  mcuxClSession_Handle_t           pSession,  */           session,
                /*  const mcuxClRsa_Key * const      pKey,  */               &private_key,
                /*  const uint8_t * const           pMessageOrDigest,  */   (uint8_t *)input,
                /*  const uint32_t                  messageLength,  */      0u,
                /*  const mcuxClRsa_SignVerifyMode   pPaddingMode,  */       (mcuxClRsa_SignVerifyMode_t *)&mcuxClRsa_Mode_Sign_NoEncode,
                /*  const uint32_t                  saltLength,  */         0u,
                /*  const uint32_t                  options,  */            0u,
                /*  uint8_t * const                 pSignature)  */         (uint8_t *)pBuf));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClRsa_sign) != sign_token) || (MCUXCLRSA_STATUS_SIGN_OK != sign_result))
    {
        if (MCUXCLRSA_STATUS_INVALID_INPUT == sign_result)
        {
            return MBEDTLS_ERR_RSA_BAD_INPUT_DATA;
        }
        else
        {
            return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
        }
    }

    session->pkcWa.used -= pkcWaSize / sizeof(uint32_t);

    /* Copy result buffer to output */
    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(retMemCpy, tokenMemCpy,
            mcuxClMemory_copy((uint8_t *) output, pBuf, nByteLength, nByteLength) );

    if ((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClMemory_copy) != tokenMemCpy) && (0u != retMemCpy) )
    {
        return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
    }

    /**************************************************************************/
    /* Session clean-up                                                       */
    /**************************************************************************/

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(cleanup_result, cleanup_token, mcuxClSession_cleanup(
                /* mcuxClSession_Handle_t           pSession: */           session));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClSession_cleanup) != cleanup_token) || (MCUXCLSESSION_STATUS_OK != cleanup_result))
    {
        return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
    }

    MCUX_CSSL_FP_FUNCTION_CALL_PROTECTED(destroy_result, destroy_token, mcuxClSession_destroy(
                /* mcuxClSession_Handle_t           pSession: */           session));

    if((MCUX_CSSL_FP_FUNCTION_CALLED(mcuxClSession_destroy) != destroy_token) || (MCUXCLSESSION_STATUS_OK != destroy_result))
    {
        return MBEDTLS_ERR_RSA_PRIVATE_FAILED;
    }

    return( 0 );
}

#endif /*!defined(MBEDTLS_RSA_CTX_ALT) || !defined(MBEDTLS_RSA_PUBLIC_ALT) || !defined(MBEDTLS_RSA_PRIVATE_ALT) */