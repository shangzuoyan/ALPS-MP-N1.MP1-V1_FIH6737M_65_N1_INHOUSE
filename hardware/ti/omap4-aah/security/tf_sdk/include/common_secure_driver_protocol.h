/*
 * Copyright (c) 2011 Trusted Logic S.A.
 * All Rights Reserved.
 *
 * This software is the confidential and proprietary information of
 * Trusted Logic S.A. ("Confidential Information"). You shall not
 * disclose such Confidential Information and shall use it only in
 * accordance with the terms of the license agreement you entered
 * into with Trusted Logic S.A.
 *
 * TRUSTED LOGIC S.A. MAKES NO REPRESENTATIONS OR WARRANTIES ABOUT THE
 * SUITABILITY OF THE SOFTWARE, EITHER EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT. TRUSTED LOGIC S.A. SHALL
 * NOT BE LIABLE FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING,
 * MODIFYING OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.
 */
#ifndef   __COMMON_SECURE_DRIVER_PROTOCOL_H__
#define   __COMMON_SECURE_DRIVER_PROTOCOL_H__

/*C2537CC3-36F0-48D9-820E-559601478029*/
#define COMMON_SECURE_DRIVER_UUID {0xC2537CC3, 0x36F0, 0x48D9, {0x82, 0x0E, 0x55, 0x96, 0x01, 0x47, 0x80, 0x29}}

#define COMMON_SECURE_DRIVER_GET_SECURE_BUFFER		0x00000000
#define COMMON_SECURE_DRIVER_HDCP_SECURE_IS_SECURE	0x00000002
#define COMMON_SECURE_DRIVER_ENTER_SECURE_PLAYBACK	0x00003000
#define COMMON_SECURE_DRIVER_EXIT_SECURE_PLAYBACK	0x00003001
#define COMMON_SECURE_DRIVER_LOCK_SECURE_PLAYBACK	0x00004000
#define COMMON_SECURE_DRIVER_UNLOCK_SECURE_PLAYBACK	0x00004001

#define COMMON_SECURE_DRIVER_CEK_UNWRAP                 0x00006000
#define COMMON_SECURE_DRIVER_KEK_WRAP                   0x00006001
#define COMMON_SECURE_DRIVER_KEK_UNWRAP                 0x00006002

#endif /* __COMMON_SECURE_DRIVER_PROTOCOL_H__ */