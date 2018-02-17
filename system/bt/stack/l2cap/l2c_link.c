/******************************************************************************
 *
 *  Copyright (C) 1999-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  this file contains the functions relating to link management. A "link"
 *  is a connection between this device and another device. Only ACL links
 *  are managed.
 *
 ******************************************************************************/

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "device/include/controller.h"
#include "bt_common.h"
#include "bt_types.h"
#include "bt_utils.h"
#include "hcimsgs.h"
#include "l2cdefs.h"
#include "l2c_int.h"
#include "l2c_api.h"
#include "btu.h"
#include "btm_api.h"
#include "btm_int.h"
#include "btcore/include/bdaddr.h"

#if (MTK_STACK_CONFIG_BL == TRUE)
#include "mediatek/include/interop_mtk.h"
#endif

extern fixed_queue_t *btu_general_alarm_queue;

static BOOLEAN l2c_link_send_to_lower (tL2C_LCB *p_lcb, BT_HDR *p_buf);

/*******************************************************************************
**
** Function         l2c_link_hci_conn_req
**
** Description      This function is called when an HCI Connection Request
**                  event is received.
**
** Returns          TRUE, if accept conn
**
*******************************************************************************/
BOOLEAN l2c_link_hci_conn_req (BD_ADDR bd_addr)
{
    tL2C_LCB        *p_lcb;
    tL2C_LCB        *p_lcb_cur;
    int             xx;
    BOOLEAN         no_links;

    /* See if we have a link control block for the remote device */
    p_lcb = l2cu_find_lcb_by_bd_addr (bd_addr, BT_TRANSPORT_BR_EDR);

    /* If we don't have one, create one and accept the connection. */
    if (!p_lcb)
    {
        p_lcb = l2cu_allocate_lcb (bd_addr, FALSE, BT_TRANSPORT_BR_EDR);
        if (!p_lcb)
        {
            btsnd_hcic_reject_conn (bd_addr, HCI_ERR_HOST_REJECT_RESOURCES);
            L2CAP_TRACE_ERROR ("L2CAP failed to allocate LCB");
            return FALSE;
        }

        no_links = TRUE;

        /* If we already have connection, accept as a master */
        for (xx = 0, p_lcb_cur = &l2cb.lcb_pool[0]; xx < MAX_L2CAP_LINKS; xx++, p_lcb_cur++)
        {
            if (p_lcb_cur == p_lcb)
                continue;

            if (p_lcb_cur->in_use)
            {
                no_links = FALSE;
                p_lcb->link_role = HCI_ROLE_MASTER;
                break;
            }
        }

        if (no_links)
        {
            if (!btm_dev_support_switch (bd_addr))
                p_lcb->link_role = HCI_ROLE_SLAVE;
            else
                p_lcb->link_role = l2cu_get_conn_role(p_lcb);
#if (MTK_STACK_CONFIG_BL == TRUE)
            if (interop_mtk_match_addr_name(INTEROP_MTK_HID_NOT_SET_SLAVE, ((const bt_bdaddr_t *)&bd_addr)))
                p_lcb->link_role = HCI_ROLE_MASTER;
#endif
        }


        /* Tell the other side we accept the connection */
        btsnd_hcic_accept_conn (bd_addr, p_lcb->link_role);

        p_lcb->link_state = LST_CONNECTING;

        /* Start a timer waiting for connect complete */
        alarm_set_on_queue(p_lcb->l2c_lcb_timer, L2CAP_LINK_CONNECT_TIMEOUT_MS,
                           l2c_lcb_timer_timeout, p_lcb,
                           btu_general_alarm_queue);
        return (TRUE);
    }

    /* We already had a link control block to the guy. Check what state it is in */
    if ((p_lcb->link_state == LST_CONNECTING) || (p_lcb->link_state == LST_CONNECT_HOLDING))
    {
        /* Connection collision. Accept the connection anyways. */

        if (!btm_dev_support_switch (bd_addr))
            p_lcb->link_role = HCI_ROLE_SLAVE;
        else
            p_lcb->link_role = l2cu_get_conn_role(p_lcb);

        btsnd_hcic_accept_conn (bd_addr, p_lcb->link_role);

        p_lcb->link_state = LST_CONNECTING;
        return (TRUE);
    }
    else if (p_lcb->link_state == LST_DISCONNECTING)
    {
        /* In disconnecting state, reject the connection. */
        btsnd_hcic_reject_conn (bd_addr, HCI_ERR_HOST_REJECT_DEVICE);
    }
    else
    {
        L2CAP_TRACE_ERROR("L2CAP got conn_req while connected (state:%d). Reject it",
                p_lcb->link_state);
        /* Reject the connection with ACL Connection Already exist reason */
        btsnd_hcic_reject_conn (bd_addr, HCI_ERR_CONNECTION_EXISTS);
    }
    return (FALSE);
}

/*******************************************************************************
**
** Function         l2c_link_hci_conn_comp
**
** Description      This function is called when an HCI Connection Complete
**                  event is received.
**
** Returns          void
**
*******************************************************************************/
BOOLEAN l2c_link_hci_conn_comp (UINT8 status, UINT16 handle, BD_ADDR p_bda)
{
    tL2C_CONN_INFO       ci;
    tL2C_LCB            *p_lcb;
    tL2C_CCB            *p_ccb;
    tBTM_SEC_DEV_REC    *p_dev_info = NULL;

    btm_acl_update_busy_level (BTM_BLI_PAGE_DONE_EVT);

    /* Save the parameters */
    ci.status       = status;
    memcpy (ci.bd_addr, p_bda, BD_ADDR_LEN);

    /* See if we have a link control block for the remote device */
    p_lcb = l2cu_find_lcb_by_bd_addr (ci.bd_addr, BT_TRANSPORT_BR_EDR);

    /* If we don't have one, this is an error */
    if (!p_lcb)
    {
        L2CAP_TRACE_WARNING ("L2CAP got conn_comp for unknown BD_ADDR");
        return (FALSE);
    }

    if (p_lcb->link_state != LST_CONNECTING)
    {
        L2CAP_TRACE_ERROR ("L2CAP got conn_comp in bad state: %d  status: 0x%d", p_lcb->link_state, status);

        if (status != HCI_SUCCESS)
            l2c_link_hci_disc_comp (p_lcb->handle, status);

        return (FALSE);
    }

    /* Save the handle */
    p_lcb->handle = handle;

    if (ci.status == HCI_SUCCESS)
    {
        /* Connected OK. Change state to connected */
        p_lcb->link_state = LST_CONNECTED;

        /* Get the peer information if the l2cap flow-control/rtrans is supported */
        l2cu_send_peer_info_req (p_lcb, L2CAP_EXTENDED_FEATURES_INFO_TYPE);

        /* Tell BTM Acl management about the link */
        if ((p_dev_info = btm_find_dev (p_bda)) != NULL)
            btm_acl_created (ci.bd_addr, p_dev_info->dev_class,
                             p_dev_info->sec_bd_name, handle,
                             p_lcb->link_role, BT_TRANSPORT_BR_EDR);
        else
            btm_acl_created (ci.bd_addr, NULL, NULL, handle, p_lcb->link_role, BT_TRANSPORT_BR_EDR);

        BTM_SetLinkSuperTout (ci.bd_addr, btm_cb.btm_def_link_super_tout);

        /* If dedicated bonding do not process any further */
        if (p_lcb->is_bonding)
        {
            if (l2cu_start_post_bond_timer(handle))
                return (TRUE);
        }

        /* Update the timeouts in the hold queue */
        l2c_process_held_packets(FALSE);

        alarm_cancel(p_lcb->l2c_lcb_timer);

        /* For all channels, send the event through their FSMs */
        for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; p_ccb = p_ccb->p_next_ccb)
        {
            l2c_csm_execute (p_ccb, L2CEVT_LP_CONNECT_CFM, &ci);
        }

        if (p_lcb->p_echo_rsp_cb)
        {
            l2cu_send_peer_echo_req (p_lcb, NULL, 0);
            alarm_set_on_queue(p_lcb->l2c_lcb_timer,
                               L2CAP_ECHO_RSP_TIMEOUT_MS,
                               l2c_lcb_timer_timeout, p_lcb,
                               btu_general_alarm_queue);
        }
        else if (!p_lcb->ccb_queue.p_first_ccb)
        {
            period_ms_t timeout_ms = L2CAP_LINK_STARTUP_TOUT * 1000;
            alarm_set_on_queue(p_lcb->l2c_lcb_timer, timeout_ms,
                               l2c_lcb_timer_timeout, p_lcb,
                               btu_general_alarm_queue);
        }
    }
    /* Max number of acl connections.                          */
    /* If there's an lcb disconnecting set this one to holding */
    else if ((ci.status == HCI_ERR_MAX_NUM_OF_CONNECTIONS) && l2cu_lcb_disconnecting())
    {
        p_lcb->link_state = LST_CONNECT_HOLDING;
        p_lcb->handle = HCI_INVALID_HANDLE;
    }
    else
    {
        /* Just in case app decides to try again in the callback context */
        p_lcb->link_state = LST_DISCONNECTING;

        /* Connection failed. For all channels, send the event through */
        /* their FSMs. The CCBs should remove themselves from the LCB  */
        for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; )
        {
            tL2C_CCB *pn = p_ccb->p_next_ccb;

            l2c_csm_execute (p_ccb, L2CEVT_LP_CONNECT_CFM_NEG, &ci);

            p_ccb = pn;
        }

        p_lcb->disc_reason = status;
        /* Release the LCB */
        if (p_lcb->ccb_queue.p_first_ccb == NULL)
            l2cu_release_lcb (p_lcb);
        else                              /* there are any CCBs remaining */
        {
            if (ci.status == HCI_ERR_CONNECTION_EXISTS)
            {
                /* we are in collision situation, wait for connecttion request from controller */
                p_lcb->link_state = LST_CONNECTING;
            }
            else
            {
                l2cu_create_conn(p_lcb, BT_TRANSPORT_BR_EDR);
            }
        }
    }
    return (TRUE);
}


/*******************************************************************************
**
** Function         l2c_link_sec_comp
**
** Description      This function is called when required security procedures
**                  are completed.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_sec_comp (BD_ADDR p_bda, tBT_TRANSPORT transport, void *p_ref_data, UINT8 status)
{
    tL2C_CONN_INFO  ci;
    tL2C_LCB        *p_lcb;
    tL2C_CCB        *p_ccb;
    tL2C_CCB        *p_next_ccb;
    UINT8           event;

    UNUSED(transport);

    L2CAP_TRACE_DEBUG ("l2c_link_sec_comp: %d, 0x%x", status, p_ref_data);

    if (status == BTM_SUCCESS_NO_SECURITY)
        status = BTM_SUCCESS;

    /* Save the parameters */
    ci.status       = status;
    memcpy (ci.bd_addr, p_bda, BD_ADDR_LEN);

    p_lcb = l2cu_find_lcb_by_bd_addr (p_bda, transport);

    /* If we don't have one, this is an error */
    if (!p_lcb)
    {
        L2CAP_TRACE_WARNING ("L2CAP got sec_comp for unknown BD_ADDR");
        return;
    }

    /* Match p_ccb with p_ref_data returned by sec manager */
    for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; p_ccb = p_next_ccb)
    {
        p_next_ccb = p_ccb->p_next_ccb;

        if (p_ccb == p_ref_data)
        {
            switch(status)
            {
            case BTM_SUCCESS:
                event = L2CEVT_SEC_COMP;
                break;

            case BTM_DELAY_CHECK:
                /* start a timer - encryption change not received before L2CAP connect req */
                alarm_set_on_queue(p_ccb->l2c_ccb_timer,
                                   L2CAP_DELAY_CHECK_SM4_TIMEOUT_MS,
                                   l2c_ccb_timer_timeout, p_ccb,
                                   btu_general_alarm_queue);
                return;

            default:
                event = L2CEVT_SEC_COMP_NEG;
            }
            l2c_csm_execute (p_ccb, event, &ci);
            break;
        }
    }
}


/*******************************************************************************
**
** Function         l2c_link_hci_disc_comp
**
** Description      This function is called when an HCI Disconnect Complete
**                  event is received.
**
** Returns          TRUE if the link is known about, else FALSE
**
*******************************************************************************/
BOOLEAN l2c_link_hci_disc_comp (UINT16 handle, UINT8 reason)
{
    tL2C_LCB    *p_lcb;
    tL2C_CCB    *p_ccb;
    BOOLEAN     status = TRUE;
    BOOLEAN     lcb_is_free = TRUE;
    tBT_TRANSPORT   transport = BT_TRANSPORT_BR_EDR;

    /* See if we have a link control block for the connection */
    p_lcb = l2cu_find_lcb_by_handle (handle);

    /* If we don't have one, maybe an SCO link. Send to MM */
    if (!p_lcb)
    {
        status = FALSE;
    }
    else
    {
        /* There can be a case when we rejected PIN code authentication */
        /* otherwise save a new reason */
        if (btm_cb.acl_disc_reason != HCI_ERR_HOST_REJECT_SECURITY)
            btm_cb.acl_disc_reason = reason;

        p_lcb->disc_reason = btm_cb.acl_disc_reason;

        /* Just in case app decides to try again in the callback context */
        p_lcb->link_state = LST_DISCONNECTING;

#if (BLE_INCLUDED == TRUE)
        /* Check for BLE and handle that differently */
        if (p_lcb->transport == BT_TRANSPORT_LE)
            btm_ble_update_link_topology_mask(p_lcb->link_role, FALSE);
#endif
        /* Link is disconnected. For all channels, send the event through */
        /* their FSMs. The CCBs should remove themselves from the LCB     */
        for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; )
        {
            tL2C_CCB *pn = p_ccb->p_next_ccb;

            /* Keep connect pending control block (if exists)
             * Possible Race condition when a reconnect occurs
             * on the channel during a disconnect of link. This
             * ccb will be automatically retried after link disconnect
             * arrives
             */
            if (p_ccb != p_lcb->p_pending_ccb)
            {
                l2c_csm_execute (p_ccb, L2CEVT_LP_DISCONNECT_IND, &reason);
            }
            p_ccb = pn;
        }

#if (BTM_SCO_INCLUDED == TRUE)
#if (BLE_INCLUDED == TRUE)
        if (p_lcb->transport == BT_TRANSPORT_BR_EDR)
#endif
            /* Tell SCO management to drop any SCOs on this ACL */
            btm_sco_acl_removed (p_lcb->remote_bd_addr);
#endif

        /* If waiting for disconnect and reconnect is pending start the reconnect now
           race condition where layer above issued connect request on link that was
           disconnecting
         */
        if (p_lcb->ccb_queue.p_first_ccb != NULL || p_lcb->p_pending_ccb)
        {
            L2CAP_TRACE_DEBUG("l2c_link_hci_disc_comp: Restarting pending ACL request");
            transport = p_lcb->transport;
#if BLE_INCLUDED == TRUE
            /* for LE link, always drop and re-open to ensure to get LE remote feature */
            if (p_lcb->transport == BT_TRANSPORT_LE)
            {
                l2cb.is_ble_connecting = FALSE;
                btm_acl_removed (p_lcb->remote_bd_addr, p_lcb->transport);
                /* Release any held buffers */
                BT_HDR *p_buf;
                while (!list_is_empty(p_lcb->link_xmit_data_q))
                {
                    p_buf = list_front(p_lcb->link_xmit_data_q);
                    list_remove(p_lcb->link_xmit_data_q, p_buf);
                    osi_free(p_buf);
                }
            }
            //For BLE link, it is also needed to release fixed channels
            //else
#endif
       {
#if (L2CAP_NUM_FIXED_CHNLS > 0)
          /* If we are going to re-use the LCB without dropping it, release all fixed channels
          here */
          int xx;
          for (xx = 0; xx < L2CAP_NUM_FIXED_CHNLS; xx++)
          {
              if (p_lcb->p_fixed_ccbs[xx] && p_lcb->p_fixed_ccbs[xx] != p_lcb->p_pending_ccb)
              {
#if BLE_INCLUDED == TRUE
                  (*l2cb.fixed_reg[xx].pL2CA_FixedConn_Cb)(xx + L2CAP_FIRST_FIXED_CHNL,
                          p_lcb->remote_bd_addr, FALSE, p_lcb->disc_reason, p_lcb->transport);
#else
                  (*l2cb.fixed_reg[xx].pL2CA_FixedConn_Cb)(xx + L2CAP_FIRST_FIXED_CHNL,
                          p_lcb->remote_bd_addr, FALSE, p_lcb->disc_reason, BT_TRANSPORT_BR_EDR);
#endif
                    if (p_lcb->p_fixed_ccbs[xx] == NULL) {
                      bdstr_t bd_addr_str = {0};
                      L2CAP_TRACE_ERROR("%s: unexpected p_fixed_ccbs[%d] is NULL remote_bd_addr = %s p_lcb = %p in_use = %d link_state = %d handle = %d link_role = %d is_bonding = %d disc_reason = %d transport = %d",
                                        __func__, xx,
                                        bdaddr_to_string((bt_bdaddr_t *)&p_lcb->remote_bd_addr,
                                                         bd_addr_str,
                                                         sizeof(bd_addr_str)),
                                        p_lcb, p_lcb->in_use,
                                        p_lcb->link_state, p_lcb->handle,
                                        p_lcb->link_role, p_lcb->is_bonding,
                                        p_lcb->disc_reason, p_lcb->transport);
                    }
                    assert(p_lcb->p_fixed_ccbs[xx] != NULL);
                    l2cu_release_ccb (p_lcb->p_fixed_ccbs[xx]);

                    p_lcb->p_fixed_ccbs[xx] = NULL;
              }
          }
#if (MTK_COMMON == TRUE)
          /* re-init link parameters */
          p_lcb->link_flush_tout = 0xFFFF;
          p_lcb->idle_timeout    = l2cb.idle_timeout;
          /* due to the link will be restarted, the timer should be canceled here. Or the lcb maybe released at timer timeout */
          alarm_cancel(p_lcb->l2c_lcb_timer);
#if (BLE_INCLUDED == TRUE)
          if (p_lcb->transport == BT_TRANSPORT_LE)
          {
              L2CAP_TRACE_DEBUG("l2c_link_hci_disc_comp: le xmit window %d, linkack %d, rr_unacked %d",
                l2cb.controller_le_xmit_window, p_lcb->sent_not_acked, l2cb.ble_round_robin_unacked);
          }
          else
#endif
          {
              L2CAP_TRACE_DEBUG("l2c_link_hci_disc_comp: xmit window %d, linkack %d, rr_unacked %d",
                l2cb.controller_xmit_window, p_lcb->sent_not_acked, l2cb.round_robin_unacked);
          }

          if (p_lcb->sent_not_acked > 0)
          {
              /* If there are some packets were not acked before disconnected,
                            resuming the xmit window here, and clearing the sent_not_acked of this lcb */

#if (BLE_INCLUDED == TRUE)
              if (p_lcb->transport == BT_TRANSPORT_LE)
              {
                  /* recover the BLE link quota and rr unacked */
                  l2cb.controller_le_xmit_window += p_lcb->sent_not_acked;
                  if (l2cb.controller_le_xmit_window > l2cb.num_lm_ble_bufs)
                  {
                      l2cb.controller_le_xmit_window = l2cb.num_lm_ble_bufs;
                  }
                  if(p_lcb->link_xmit_quota == 0)
                  {
                      if (l2cb.ble_round_robin_unacked > p_lcb->sent_not_acked)
                          l2cb.ble_round_robin_unacked -= p_lcb->sent_not_acked;
                      else
                          l2cb.ble_round_robin_unacked = 0;
                  }
              }
              else
#endif
              {
                  /* recover the BR/EDR link quota and rr unacked */
                  l2cb.controller_xmit_window += p_lcb->sent_not_acked;
                  if (l2cb.controller_xmit_window > l2cb.num_lm_acl_bufs)
                  {
                      l2cb.controller_xmit_window = l2cb.num_lm_acl_bufs;
                  }
                  if(p_lcb->link_xmit_quota == 0)
                  {
                      if (l2cb.round_robin_unacked > p_lcb->sent_not_acked)
                          l2cb.round_robin_unacked -= p_lcb->sent_not_acked;
                      else
                          l2cb.round_robin_unacked = 0;
                  }
              }
              p_lcb->sent_not_acked = 0;
#if (BLE_INCLUDED == TRUE)
              if (p_lcb->transport == BT_TRANSPORT_LE)
              {
                  L2CAP_TRACE_DEBUG("l2c_link_hci_disc_comp: after resuming, le xmit window %d, linkack %d, rr_unacked %d",
                    l2cb.controller_le_xmit_window, p_lcb->sent_not_acked, l2cb.ble_round_robin_unacked);
              }
              else
#endif
              {
                  L2CAP_TRACE_DEBUG("l2c_link_hci_disc_comp: after resuming, xmit window %d, linkack %d, rr_unacked %d",
                    l2cb.controller_xmit_window, p_lcb->sent_not_acked, l2cb.round_robin_unacked);
              }
          }
#endif
#endif
        }
            if (l2cu_create_conn(p_lcb, transport))
                lcb_is_free = FALSE; /* still using this lcb */
        }

        p_lcb->p_pending_ccb = NULL;

        /* Release the LCB */
        if (lcb_is_free)
            l2cu_release_lcb (p_lcb);
    }

    /* Now that we have a free acl connection, see if any lcbs are pending */
    if (lcb_is_free && ((p_lcb = l2cu_find_lcb_by_state(LST_CONNECT_HOLDING)) != NULL))
    {
        /* we found one-- create a connection */
        l2cu_create_conn(p_lcb, BT_TRANSPORT_BR_EDR);
    }

    return status;
}


/*******************************************************************************
**
** Function         l2c_link_hci_qos_violation
**
** Description      This function is called when an HCI QOS Violation
**                  event is received.
**
** Returns          TRUE if the link is known about, else FALSE
**
*******************************************************************************/
BOOLEAN l2c_link_hci_qos_violation (UINT16 handle)
{
    tL2C_LCB        *p_lcb;
    tL2C_CCB        *p_ccb;

    /* See if we have a link control block for the connection */
    p_lcb = l2cu_find_lcb_by_handle (handle);

    /* If we don't have one, maybe an SCO link. */
    if (!p_lcb)
        return (FALSE);

    /* For all channels, tell the upper layer about it */
    for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; p_ccb = p_ccb->p_next_ccb)
    {
        if (p_ccb->p_rcb->api.pL2CA_QoSViolationInd_Cb)
            l2c_csm_execute (p_ccb, L2CEVT_LP_QOS_VIOLATION_IND, NULL);
    }

    return (TRUE);
}



/*******************************************************************************
**
** Function         l2c_link_timeout
**
** Description      This function is called when a link timer expires
**
** Returns          void
**
*******************************************************************************/
void l2c_link_timeout (tL2C_LCB *p_lcb)
{
    tL2C_CCB   *p_ccb;
    tBTM_STATUS rc;

     L2CAP_TRACE_EVENT ("L2CAP - l2c_link_timeout() link state %d first CCB %p is_bonding:%d",
         p_lcb->link_state, p_lcb->ccb_queue.p_first_ccb, p_lcb->is_bonding);

    /* If link was connecting or disconnecting, clear all channels and drop the LCB */
    if ((p_lcb->link_state == LST_CONNECTING_WAIT_SWITCH) ||
        (p_lcb->link_state == LST_CONNECTING) ||
        (p_lcb->link_state == LST_CONNECT_HOLDING) ||
        (p_lcb->link_state == LST_DISCONNECTING))
    {
        p_lcb->p_pending_ccb = NULL;

        /* For all channels, send a disconnect indication event through */
        /* their FSMs. The CCBs should remove themselves from the LCB   */
        for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; )
        {
            tL2C_CCB *pn = p_ccb->p_next_ccb;

            l2c_csm_execute (p_ccb, L2CEVT_LP_DISCONNECT_IND, NULL);

            p_ccb = pn;
        }
#if (BLE_INCLUDED == TRUE)
        if (p_lcb->link_state == LST_CONNECTING &&
            l2cb.is_ble_connecting == TRUE)
        {
            L2CA_CancelBleConnectReq(l2cb.ble_connecting_bda);
        }
#endif
        /* Release the LCB */
        l2cu_release_lcb (p_lcb);
    }

    /* If link is connected, check for inactivity timeout */
    if (p_lcb->link_state == LST_CONNECTED)
    {
        /* Check for ping outstanding */
        if (p_lcb->p_echo_rsp_cb)
        {
            tL2CA_ECHO_RSP_CB *p_cb = p_lcb->p_echo_rsp_cb;

            /* Zero out the callback in case app immediately calls us again */
            p_lcb->p_echo_rsp_cb = NULL;

            (*p_cb) (L2CAP_PING_RESULT_NO_RESP);

             L2CAP_TRACE_WARNING ("L2CAP - ping timeout");

            /* For all channels, send a disconnect indication event through */
            /* their FSMs. The CCBs should remove themselves from the LCB   */
            for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; )
            {
                tL2C_CCB *pn = p_ccb->p_next_ccb;

                l2c_csm_execute (p_ccb, L2CEVT_LP_DISCONNECT_IND, NULL);

                p_ccb = pn;
            }
        }

        /* If no channels in use, drop the link. */
        if (!p_lcb->ccb_queue.p_first_ccb)
        {
            period_ms_t timeout_ms;
            bool start_timeout = true;

            rc = btm_sec_disconnect (p_lcb->handle, HCI_ERR_PEER_USER);

            if (rc == BTM_CMD_STORED)
            {
                /* Security Manager will take care of disconnecting, state will be updated at that time */
                start_timeout = false;
            }
            else if (rc == BTM_CMD_STARTED)
            {
                p_lcb->link_state = LST_DISCONNECTING;
                timeout_ms = L2CAP_LINK_DISCONNECT_TIMEOUT_MS;
            }
            else if (rc == BTM_SUCCESS)
            {
                l2cu_process_fixed_disc_cback(p_lcb);
                /* BTM SEC will make sure that link is release (probably after pairing is done) */
                p_lcb->link_state = LST_DISCONNECTING;
                start_timeout = false;
            }
            else if (rc == BTM_BUSY)
            {
                /* BTM is still executing security process. Let lcb stay as connected */
                start_timeout = false;
            }
            else if ((p_lcb->is_bonding)
                  && (btsnd_hcic_disconnect (p_lcb->handle, HCI_ERR_PEER_USER)))
            {
                l2cu_process_fixed_disc_cback(p_lcb);
                p_lcb->link_state = LST_DISCONNECTING;
                timeout_ms = L2CAP_LINK_DISCONNECT_TIMEOUT_MS;
            }
            else
            {
                /* probably no buffer to send disconnect */
                timeout_ms = BT_1SEC_TIMEOUT_MS;
            }

            if (start_timeout) {
                alarm_set_on_queue(p_lcb->l2c_lcb_timer, timeout_ms,
                                   l2c_lcb_timer_timeout, p_lcb,
                                   btu_general_alarm_queue);
            }
        }
        else
        {
            /* Check in case we were flow controlled */
            l2c_link_check_send_pkts (p_lcb, NULL, NULL);
        }
    }
}

/*******************************************************************************
**
** Function         l2c_info_resp_timer_timeout
**
** Description      This function is called when an info request times out
**
** Returns          void
**
*******************************************************************************/
void l2c_info_resp_timer_timeout(void *data)
{
    tL2C_LCB *p_lcb = (tL2C_LCB *)data;
    tL2C_CCB   *p_ccb;
    tL2C_CONN_INFO  ci;

    /* If we timed out waiting for info response, just continue using basic if allowed */
    if (p_lcb->w4_info_rsp)
    {
        /* If waiting for security complete, restart the info response timer */
        for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; p_ccb = p_ccb->p_next_ccb)
        {
            if ( (p_ccb->chnl_state == CST_ORIG_W4_SEC_COMP) || (p_ccb->chnl_state == CST_TERM_W4_SEC_COMP) )
            {
                alarm_set_on_queue(p_lcb->info_resp_timer,
                                   L2CAP_WAIT_INFO_RSP_TIMEOUT_MS,
                                   l2c_info_resp_timer_timeout, p_lcb,
                                   btu_general_alarm_queue);
                return;
            }
        }

        p_lcb->w4_info_rsp = FALSE;

        /* If link is in process of being brought up */
        if ((p_lcb->link_state != LST_DISCONNECTED) &&
            (p_lcb->link_state != LST_DISCONNECTING))
        {
            /* Notify active channels that peer info is finished */
            if (p_lcb->ccb_queue.p_first_ccb)
            {
                ci.status = HCI_SUCCESS;
                memcpy (ci.bd_addr, p_lcb->remote_bd_addr, sizeof(BD_ADDR));

                for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; p_ccb = p_ccb->p_next_ccb)
                {
                    l2c_csm_execute (p_ccb, L2CEVT_L2CAP_INFO_RSP, &ci);
                }
            }
        }
    }
}

/*******************************************************************************
**
** Function         l2c_link_adjust_allocation
**
** Description      This function is called when a link is created or removed
**                  to calculate the amount of packets each link may send to
**                  the HCI without an ack coming back.
**
**                  Currently, this is a simple allocation, dividing the
**                  number of Controller Packets by the number of links. In
**                  the future, QOS configuration should be examined.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_adjust_allocation (void)
{
    UINT16      qq, yy, qq_remainder;
    tL2C_LCB    *p_lcb;
    UINT16      hi_quota, low_quota;
    UINT16      num_lowpri_links = 0;
    UINT16      num_hipri_links  = 0;
    UINT16      controller_xmit_quota = l2cb.num_lm_acl_bufs;
    UINT16      high_pri_link_quota = L2CAP_HIGH_PRI_MIN_XMIT_QUOTA_A;

    /* If no links active, reset buffer quotas and controller buffers */
    if (l2cb.num_links_active == 0)
    {
        l2cb.controller_xmit_window = l2cb.num_lm_acl_bufs;
        l2cb.round_robin_quota = l2cb.round_robin_unacked = 0;
        return;
    }

    /* First, count the links */
    for (yy = 0, p_lcb = &l2cb.lcb_pool[0]; yy < MAX_L2CAP_LINKS; yy++, p_lcb++)
    {
        if (p_lcb->in_use && p_lcb->transport == BT_TRANSPORT_BR_EDR)
        {
            if (p_lcb->acl_priority == L2CAP_PRIORITY_HIGH)
                num_hipri_links++;
            else
                num_lowpri_links++;
        }
    }

    /* now adjust high priority link quota */
    low_quota = num_lowpri_links ? 1 : 0;
    while ( (num_hipri_links * high_pri_link_quota + low_quota) > controller_xmit_quota )
        high_pri_link_quota--;

    /* Work out the xmit quota and buffer quota high and low priorities */
    hi_quota  = num_hipri_links * high_pri_link_quota;
    low_quota = (hi_quota < controller_xmit_quota) ? controller_xmit_quota - hi_quota : 1;

    /* Work out and save the HCI xmit quota for each low priority link */

    /* If each low priority link cannot have at least one buffer */
    if (num_lowpri_links > low_quota)
    {
        l2cb.round_robin_quota = low_quota;
        qq = qq_remainder = 0;
    }
    /* If each low priority link can have at least one buffer */
    else if (num_lowpri_links > 0)
    {
        l2cb.round_robin_quota = 0;
        l2cb.round_robin_unacked = 0;
        qq = low_quota / num_lowpri_links;
        qq_remainder = low_quota % num_lowpri_links;
    }
    /* If no low priority link */
    else
    {
        l2cb.round_robin_quota = 0;
        l2cb.round_robin_unacked = 0;
        qq = qq_remainder = 0;
    }

    L2CAP_TRACE_EVENT ("l2c_link_adjust_allocation  num_hipri: %u  num_lowpri: %u  low_quota: %u  round_robin_quota: %u  qq: %u",
                        num_hipri_links, num_lowpri_links, low_quota,
                        l2cb.round_robin_quota, qq);

    /* Now, assign the quotas to each link */
    for (yy = 0, p_lcb = &l2cb.lcb_pool[0]; yy < MAX_L2CAP_LINKS; yy++, p_lcb++)
    {
        if (p_lcb->in_use && p_lcb->transport == BT_TRANSPORT_BR_EDR)
        {
            if (p_lcb->acl_priority == L2CAP_PRIORITY_HIGH)
            {
                p_lcb->link_xmit_quota   = high_pri_link_quota;
            }
            else
            {
                /* Safety check in case we switched to round-robin with something outstanding */
                /* if sent_not_acked is added into round_robin_unacked then don't add it again */
                /* l2cap keeps updating sent_not_acked for exiting from round robin */
                if (( p_lcb->link_xmit_quota > 0 )&&( qq == 0 ))
                    l2cb.round_robin_unacked += p_lcb->sent_not_acked;

                p_lcb->link_xmit_quota   = qq;
                if (qq_remainder > 0)
                {
                    p_lcb->link_xmit_quota++;
                    qq_remainder--;
                }
            }

            L2CAP_TRACE_EVENT ("l2c_link_adjust_allocation LCB %d   Priority: %d  XmitQuota: %d",
                                yy, p_lcb->acl_priority, p_lcb->link_xmit_quota);

            L2CAP_TRACE_EVENT ("        SentNotAcked: %d  RRUnacked: %d",
                                p_lcb->sent_not_acked, l2cb.round_robin_unacked);

            /* There is a special case where we have readjusted the link quotas and  */
            /* this link may have sent anything but some other link sent packets so  */
            /* so we may need a timer to kick off this link's transmissions.         */
            if ( (p_lcb->link_state == LST_CONNECTED)
              && (!list_is_empty(p_lcb->link_xmit_data_q))
                 && (p_lcb->sent_not_acked < p_lcb->link_xmit_quota) ) {
                alarm_set_on_queue(p_lcb->l2c_lcb_timer,
                                   L2CAP_LINK_FLOW_CONTROL_TIMEOUT_MS,
                                   l2c_lcb_timer_timeout, p_lcb,
                                   btu_general_alarm_queue);
            }
        }
    }
}

/*******************************************************************************
**
** Function         l2c_link_adjust_chnl_allocation
**
** Description      This function is called to calculate the amount of packets each
**                  non-F&EC channel may have outstanding.
**
**                  Currently, this is a simple allocation, dividing the number
**                  of packets allocated to the link by the number of channels. In
**                  the future, QOS configuration should be examined.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_adjust_chnl_allocation (void)
{
    UINT8       xx;

    L2CAP_TRACE_DEBUG("%s", __func__);

    /* assign buffer quota to each channel based on its data rate requirement */
    for (xx = 0; xx < MAX_L2CAP_CHANNELS; xx++)
    {
        tL2C_CCB *p_ccb = l2cb.ccb_pool + xx;

        if (!p_ccb->in_use)
            continue;

        tL2CAP_CHNL_DATA_RATE data_rate = p_ccb->tx_data_rate + p_ccb->rx_data_rate;
        p_ccb->buff_quota = L2CAP_CBB_DEFAULT_DATA_RATE_BUFF_QUOTA * data_rate;
        L2CAP_TRACE_EVENT("CID:0x%04x FCR Mode:%u Priority:%u TxDataRate:%u RxDataRate:%u Quota:%u",
                          p_ccb->local_cid, p_ccb->peer_cfg.fcr.mode,
                          p_ccb->ccb_priority, p_ccb->tx_data_rate,
                          p_ccb->rx_data_rate, p_ccb->buff_quota);

        /* quota may be change so check congestion */
        l2cu_check_channel_congestion(p_ccb);
    }
}

/*******************************************************************************
**
** Function         l2c_link_processs_num_bufs
**
** Description      This function is called when a "controller buffer size"
**                  event is first received from the controller. It updates
**                  the L2CAP values.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_processs_num_bufs (UINT16 num_lm_acl_bufs)
{
    l2cb.num_lm_acl_bufs = l2cb.controller_xmit_window = num_lm_acl_bufs;

}

/*******************************************************************************
**
** Function         l2c_link_pkts_rcvd
**
** Description      This function is called from the HCI transport when it is time
**                  tto send a "Host ready for packets" command. This is only when
**                  host to controller flow control is used. If fills in the arrays
**                  of numbers of packets and handles.
**
** Returns          count of number of entries filled in
**
*******************************************************************************/
UINT8 l2c_link_pkts_rcvd (UINT16 *num_pkts, UINT16 *handles)
{
    UINT8       num_found = 0;

    UNUSED(num_pkts);
    UNUSED(handles);

    return (num_found);
}

/*******************************************************************************
**
** Function         l2c_link_role_changed
**
** Description      This function is called whan a link's master/slave role change
**                  event is received. It simply updates the link control block.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_role_changed (BD_ADDR bd_addr, UINT8 new_role, UINT8 hci_status)
{
    tL2C_LCB *p_lcb;
    int      xx;

    /* Make sure not called from HCI Command Status (bd_addr and new_role are invalid) */
    if (bd_addr)
    {
        /* If here came form hci role change event */
        p_lcb = l2cu_find_lcb_by_bd_addr (bd_addr, BT_TRANSPORT_BR_EDR);
        if (p_lcb)
        {
            p_lcb->link_role = new_role;

            /* Reset high priority link if needed */
            if (hci_status == HCI_SUCCESS)
                l2cu_set_acl_priority(bd_addr, p_lcb->acl_priority, TRUE);
        }
    }

    /* Check if any LCB was waiting for switch to be completed */
    for (xx = 0, p_lcb = &l2cb.lcb_pool[0]; xx < MAX_L2CAP_LINKS; xx++, p_lcb++)
    {
        if ((p_lcb->in_use) && (p_lcb->link_state == LST_CONNECTING_WAIT_SWITCH))
        {
            l2cu_create_conn_after_switch (p_lcb);
        }
    }
}

/*******************************************************************************
**
** Function         l2c_pin_code_request
**
** Description      This function is called whan a pin-code request is received
**                  on a connection. If there are no channels active yet on the
**                  link, it extends the link first connection timer.  Make sure
**                  that inactivity timer is not extended if PIN code happens
**                  to be after last ccb released.
**
** Returns          void
**
*******************************************************************************/
void l2c_pin_code_request (BD_ADDR bd_addr)
{
    tL2C_LCB *p_lcb = l2cu_find_lcb_by_bd_addr (bd_addr, BT_TRANSPORT_BR_EDR);

    if ( (p_lcb) && (!p_lcb->ccb_queue.p_first_ccb) )
    {
        alarm_set_on_queue(p_lcb->l2c_lcb_timer,
                           L2CAP_LINK_CONNECT_EXT_TIMEOUT_MS,
                           l2c_lcb_timer_timeout, p_lcb,
                           btu_general_alarm_queue);
    }
}

#if L2CAP_WAKE_PARKED_LINK == TRUE
/*******************************************************************************
**
** Function         l2c_link_check_power_mode
**
** Description      This function is called to check power mode.
**
** Returns          TRUE if link is going to be active from park
**                  FALSE if nothing to send or not in park mode
**
*******************************************************************************/
BOOLEAN l2c_link_check_power_mode (tL2C_LCB *p_lcb)
{
    tBTM_PM_MODE     mode;
    tL2C_CCB    *p_ccb;
    BOOLEAN need_to_active = FALSE;

    /*
     * We only switch park to active only if we have unsent packets
     */
    if (list_is_empty(p_lcb->link_xmit_data_q))
    {
        for (p_ccb = p_lcb->ccb_queue.p_first_ccb; p_ccb; p_ccb = p_ccb->p_next_ccb)
        {
            if (!fixed_queue_is_empty(p_ccb->xmit_hold_q))
            {
                need_to_active = TRUE;
                break;
            }
        }
    }
    else
        need_to_active = TRUE;

    /* if we have packets to send */
    if ( need_to_active )
    {
        /* check power mode */
        if (BTM_ReadPowerMode(p_lcb->remote_bd_addr, &mode) == BTM_SUCCESS)
        {
            if ( mode == BTM_PM_STS_PENDING )
            {
                L2CAP_TRACE_DEBUG ("LCB(0x%x) is in PM pending state", p_lcb->handle);

                return TRUE;
            }
        }
    }
    return FALSE;
}
#endif /* L2CAP_WAKE_PARKED_LINK == TRUE) */

/*******************************************************************************
**
** Function         l2c_link_check_send_pkts
**
** Description      This function is called to check if it can send packets
**                  to the Host Controller. It may be passed the address of
**                  a packet to send.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_check_send_pkts (tL2C_LCB *p_lcb, tL2C_CCB *p_ccb, BT_HDR *p_buf)
{
    int         xx;
    BOOLEAN     single_write = FALSE;
#if (MTK_COMMON == TRUE)
    UINT16 fixd_cid = 0;
#endif

    /* Save the channel ID for faster counting */
    if (p_buf)
    {
        if (p_ccb != NULL)
        {
            p_buf->event = p_ccb->local_cid;
            single_write = TRUE;
        }
        else
            p_buf->event = 0;

        p_buf->layer_specific = 0;
        list_append(p_lcb->link_xmit_data_q, p_buf);

        if (p_lcb->link_xmit_quota == 0)
        {
#if BLE_INCLUDED == TRUE
            if (p_lcb->transport == BT_TRANSPORT_LE)
                l2cb.ble_check_round_robin = TRUE;
            else
#endif
                l2cb.check_round_robin = TRUE;
        }
    }

    /* If this is called from uncongested callback context break recursive calling.
    ** This LCB will be served when receiving number of completed packet event.
    */
    if (l2cb.is_cong_cback_context)
        return;

    /* If we are in a scenario where there are not enough buffers for each link to
    ** have at least 1, then do a round-robin for all the LCBs
    */
    if ( (p_lcb == NULL) || (p_lcb->link_xmit_quota == 0) )
    {
        if (p_lcb == NULL)
            p_lcb = l2cb.lcb_pool;
        else if (!single_write)
            p_lcb++;

        /* Loop through, starting at the next */
        for (xx = 0; xx < MAX_L2CAP_LINKS; xx++, p_lcb++)
        {
#if (MTK_COMMON == TRUE)
            /* Check for wraparound */
            if (p_lcb == &l2cb.lcb_pool[MAX_L2CAP_LINKS])
            {
                p_lcb = &l2cb.lcb_pool[0];
            }
#endif

            /* If controller window is full, nothing to do */
            if (((l2cb.controller_xmit_window == 0 ||
                  (l2cb.round_robin_unacked >= l2cb.round_robin_quota))
#if (BLE_INCLUDED == TRUE)
                && (p_lcb->transport == BT_TRANSPORT_BR_EDR)
                )
              || (p_lcb->transport == BT_TRANSPORT_LE &&
                 (l2cb.ble_round_robin_unacked >= l2cb.ble_round_robin_quota ||
                  l2cb.controller_le_xmit_window == 0 )))
#else
                ))
#endif
#if (BLE_INCLUDED == TRUE)
            continue;
#else
            break;
#endif

#if (MTK_COMMON == FALSE)
            /* Check for wraparound */
            if (p_lcb == &l2cb.lcb_pool[MAX_L2CAP_LINKS])
                p_lcb = &l2cb.lcb_pool[0];

#endif
            if ( (!p_lcb->in_use)
               || (p_lcb->partial_segment_being_sent)
               || (p_lcb->link_state != LST_CONNECTED)
               || (p_lcb->link_xmit_quota != 0)
               || (L2C_LINK_CHECK_POWER_MODE (p_lcb)) )
                continue;

            /* See if we can send anything from the Link Queue */
            if (!list_is_empty(p_lcb->link_xmit_data_q)) {
                p_buf = (BT_HDR *)list_front(p_lcb->link_xmit_data_q);
                list_remove(p_lcb->link_xmit_data_q, p_buf);
                l2c_link_send_to_lower (p_lcb, p_buf);
            }
            else if (single_write)
            {
                /* If only doing one write, break out */
                break;
            }
            /* If nothing on the link queue, check the channel queue */
#if (MTK_COMMON == TRUE)
            else if ((p_buf = l2cu_get_next_buffer_to_send (p_lcb, &fixd_cid)) != NULL)
#else
            else if ((p_buf = l2cu_get_next_buffer_to_send (p_lcb)) != NULL)
#endif
            {
                l2c_link_send_to_lower (p_lcb, p_buf);
#if (MTK_COMMON == TRUE)
                if(0 != fixd_cid)
                {
                    L2CAP_TRACE_DEBUG("l2c_link_check_send_pkts: rr fixed_cid = %d, send tx complete", fixd_cid);
                    /* send tx complete */
                    if (l2cb.fixed_reg[fixd_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedTxComplete_Cb)
                    {
                        (*l2cb.fixed_reg[fixd_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedTxComplete_Cb)(fixd_cid, 1);
                    }
                }
#endif
            }
        }

        /* If we finished without using up our quota, no need for a safety check */
        if ( (l2cb.controller_xmit_window > 0)
          && (l2cb.round_robin_unacked < l2cb.round_robin_quota)
#if (BLE_INCLUDED == TRUE)
          && (p_lcb->transport == BT_TRANSPORT_BR_EDR)
#endif
          )
            l2cb.check_round_robin = FALSE;

#if (BLE_INCLUDED == TRUE)
        if ( (l2cb.controller_le_xmit_window > 0)
          && (l2cb.ble_round_robin_unacked < l2cb.ble_round_robin_quota)
          && (p_lcb->transport == BT_TRANSPORT_LE))
            l2cb.ble_check_round_robin = FALSE;
#endif
    }
    else /* if this is not round-robin service */
    {
        /* If a partial segment is being sent, can't send anything else */
        if ( (p_lcb->partial_segment_being_sent)
          || (p_lcb->link_state != LST_CONNECTED)
          || (L2C_LINK_CHECK_POWER_MODE (p_lcb)) )
            return;

        if(p_lcb->transport != BT_TRANSPORT_BR_EDR && p_lcb->transport != BT_TRANSPORT_LE)
            L2CAP_TRACE_DEBUG ("l2c_link_check_send_pkts: transport = %d", p_lcb->transport);
        /* See if we can send anything from the link queue */
#if (BLE_INCLUDED == TRUE)
        while ( ((l2cb.controller_xmit_window != 0 && (p_lcb->transport == BT_TRANSPORT_BR_EDR)) ||
                 (l2cb.controller_le_xmit_window != 0 && (p_lcb->transport == BT_TRANSPORT_LE)))
             && (p_lcb->sent_not_acked < p_lcb->link_xmit_quota))
#else
        while ( (l2cb.controller_xmit_window != 0)
             && (p_lcb->sent_not_acked < p_lcb->link_xmit_quota))
#endif
        {
            if (list_is_empty(p_lcb->link_xmit_data_q))
                break;

            p_buf = (BT_HDR *)list_front(p_lcb->link_xmit_data_q);
            list_remove(p_lcb->link_xmit_data_q, p_buf);
            if (!l2c_link_send_to_lower (p_lcb, p_buf))
                break;
        }

        if (!single_write)
        {
            /* See if we can send anything for any channel */
#if (BLE_INCLUDED == TRUE)
            while ( ((l2cb.controller_xmit_window != 0 && (p_lcb->transport == BT_TRANSPORT_BR_EDR)) ||
                    (l2cb.controller_le_xmit_window != 0 && (p_lcb->transport == BT_TRANSPORT_LE)))
                    && (p_lcb->sent_not_acked < p_lcb->link_xmit_quota))
#else
            while ((l2cb.controller_xmit_window != 0) && (p_lcb->sent_not_acked < p_lcb->link_xmit_quota))
#endif
            {
#if (MTK_COMMON == TRUE)
                if ((p_buf = l2cu_get_next_buffer_to_send (p_lcb, &fixd_cid)) == NULL)
#else
                if ((p_buf = l2cu_get_next_buffer_to_send (p_lcb)) == NULL)
#endif
                {
                    break;
                }

                if (!l2c_link_send_to_lower (p_lcb, p_buf))
                {
                    break;
                }
#if (MTK_COMMON == TRUE)
                if(0 != fixd_cid)
                {
                    L2CAP_TRACE_DEBUG("l2c_link_check_send_pkts: fixed_cid = %d, send tx complete", fixd_cid);
                    /* send tx complete */
                    if (l2cb.fixed_reg[fixd_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedTxComplete_Cb)
                    {
                        (*l2cb.fixed_reg[fixd_cid - L2CAP_FIRST_FIXED_CHNL].pL2CA_FixedTxComplete_Cb)(fixd_cid, 1);
                    }
                }
#endif
            }
        }

        /* There is a special case where we have readjusted the link quotas and  */
        /* this link may have sent anything but some other link sent packets so  */
        /* so we may need a timer to kick off this link's transmissions.         */
        if ( (!list_is_empty(p_lcb->link_xmit_data_q)) && (p_lcb->sent_not_acked < p_lcb->link_xmit_quota) ) {
            alarm_set_on_queue(p_lcb->l2c_lcb_timer,
                               L2CAP_LINK_FLOW_CONTROL_TIMEOUT_MS,
                               l2c_lcb_timer_timeout, p_lcb,
                               btu_general_alarm_queue);
        }
    }
}

/*******************************************************************************
**
** Function         l2c_link_send_to_lower
**
** Description      This function queues the buffer for HCI transmission
**
** Returns          TRUE for success, FALSE for fail
**
*******************************************************************************/
static BOOLEAN l2c_link_send_to_lower (tL2C_LCB *p_lcb, BT_HDR *p_buf)
{
    UINT16      num_segs;
    UINT16      xmit_window, acl_data_size;
    const controller_t *controller = controller_get_interface();

    if ((p_buf->len <= controller->get_acl_packet_size_classic()
#if (BLE_INCLUDED == TRUE)
        && (p_lcb->transport == BT_TRANSPORT_BR_EDR)) ||
        ((p_lcb->transport == BT_TRANSPORT_LE) && (p_buf->len <= controller->get_acl_packet_size_ble()))
#else
        )
#endif
        )
    {
        if (p_lcb->link_xmit_quota == 0)
        {
#if (BLE_INCLUDED == TRUE)
            if (p_lcb->transport == BT_TRANSPORT_LE)
                l2cb.ble_round_robin_unacked++;
            else
#endif
                l2cb.round_robin_unacked++;
        }
        p_lcb->sent_not_acked++;
        p_buf->layer_specific = 0;

#if (BLE_INCLUDED == TRUE)
        if (p_lcb->transport == BT_TRANSPORT_LE)
        {
            l2cb.controller_le_xmit_window--;
            bte_main_hci_send(p_buf, (UINT16)(BT_EVT_TO_LM_HCI_ACL|LOCAL_BLE_CONTROLLER_ID));
        }
        else
#endif
        {
            l2cb.controller_xmit_window--;
            bte_main_hci_send(p_buf, BT_EVT_TO_LM_HCI_ACL);
        }
    }
    else
    {
#if BLE_INCLUDED == TRUE
        if (p_lcb->transport == BT_TRANSPORT_LE)
        {
            acl_data_size = controller->get_acl_data_size_ble();
            xmit_window = l2cb.controller_le_xmit_window;

        }
        else
#endif
        {
            acl_data_size = controller->get_acl_data_size_classic();
            xmit_window = l2cb.controller_xmit_window;
        }
        num_segs = (p_buf->len - HCI_DATA_PREAMBLE_SIZE + acl_data_size - 1) / acl_data_size;


        /* If doing round-robin, then only 1 segment each time */
        if (p_lcb->link_xmit_quota == 0)
        {
            num_segs = 1;
            p_lcb->partial_segment_being_sent = TRUE;
        }
        else
        {
            /* Multi-segment packet. Make sure it can fit */
            if (num_segs > xmit_window)
            {
                num_segs = xmit_window;
                p_lcb->partial_segment_being_sent = TRUE;
            }

            if (num_segs > (p_lcb->link_xmit_quota - p_lcb->sent_not_acked))
            {
                num_segs = (p_lcb->link_xmit_quota - p_lcb->sent_not_acked);
                p_lcb->partial_segment_being_sent = TRUE;
            }
        }

        p_buf->layer_specific        = num_segs;
#if BLE_INCLUDED == TRUE
        if (p_lcb->transport == BT_TRANSPORT_LE)
        {
            l2cb.controller_le_xmit_window -= num_segs;
            if (p_lcb->link_xmit_quota == 0)
                l2cb.ble_round_robin_unacked += num_segs;
        }
        else
#endif
        {
            l2cb.controller_xmit_window -= num_segs;

            if (p_lcb->link_xmit_quota == 0)
                l2cb.round_robin_unacked += num_segs;
        }

        p_lcb->sent_not_acked += num_segs;
#if BLE_INCLUDED == TRUE
        if (p_lcb->transport == BT_TRANSPORT_LE)
        {
            bte_main_hci_send(p_buf, (UINT16)(BT_EVT_TO_LM_HCI_ACL|LOCAL_BLE_CONTROLLER_ID));
        }
        else
#endif
        {
            bte_main_hci_send(p_buf, BT_EVT_TO_LM_HCI_ACL);
        }
    }

#if (L2CAP_HCI_FLOW_CONTROL_DEBUG == TRUE)
#if (BLE_INCLUDED == TRUE)
    if (p_lcb->transport == BT_TRANSPORT_LE)
    {
        L2CAP_TRACE_DEBUG ("TotalWin=%d,Hndl=0x%x,Quota=%d,Unack=%d,RRQuota=%d,RRUnack=%d",
                l2cb.controller_le_xmit_window,
                p_lcb->handle,
                p_lcb->link_xmit_quota, p_lcb->sent_not_acked,
                l2cb.ble_round_robin_quota, l2cb.ble_round_robin_unacked);
    }
    else
#endif
    {
        L2CAP_TRACE_DEBUG ("TotalWin=%d,Hndl=0x%x,Quota=%d,Unack=%d,RRQuota=%d,RRUnack=%d",
                l2cb.controller_xmit_window,
                p_lcb->handle,
                p_lcb->link_xmit_quota, p_lcb->sent_not_acked,
                l2cb.round_robin_quota, l2cb.round_robin_unacked);
    }
#endif

    return TRUE;
}

/*******************************************************************************
**
** Function         l2c_link_process_num_completed_pkts
**
** Description      This function is called when a "number-of-completed-packets"
**                  event is received from the controller. It updates all the
**                  LCB transmit counts.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_process_num_completed_pkts (UINT8 *p)
{
    UINT8       num_handles, xx;
    UINT16      handle;
    UINT16      num_sent;
    tL2C_LCB    *p_lcb;

    STREAM_TO_UINT8 (num_handles, p);

    for (xx = 0; xx < num_handles; xx++)
    {
        STREAM_TO_UINT16 (handle, p);
        STREAM_TO_UINT16 (num_sent, p);

        p_lcb = l2cu_find_lcb_by_handle (handle);

        /* Callback for number of completed packet event    */
        /* Originally designed for [3DSG]                   */
        if((p_lcb != NULL) && (p_lcb->p_nocp_cb))
        {
            L2CAP_TRACE_DEBUG ("L2CAP - calling NoCP callback");
            (*p_lcb->p_nocp_cb)(p_lcb->remote_bd_addr);
        }

        if (p_lcb)
        {
#if (BLE_INCLUDED == TRUE)
        if (p_lcb && (p_lcb->transport == BT_TRANSPORT_LE))
            l2cb.controller_le_xmit_window += num_sent;
        else
#endif
            {
                /* Maintain the total window to the controller */
                l2cb.controller_xmit_window += num_sent;
            }
            /* If doing round-robin, adjust communal counts */
            if (p_lcb->link_xmit_quota == 0)
            {
#if BLE_INCLUDED == TRUE
                if (p_lcb->transport == BT_TRANSPORT_LE)
                {
                   /* Don't go negative */
                    if (l2cb.ble_round_robin_unacked > num_sent)
                        l2cb.ble_round_robin_unacked -= num_sent;
                    else
                        l2cb.ble_round_robin_unacked = 0;
                }
                else
#endif
                {
                    /* Don't go negative */
                    if (l2cb.round_robin_unacked > num_sent)
                        l2cb.round_robin_unacked -= num_sent;
                    else
                        l2cb.round_robin_unacked = 0;
                }
            }

            /* Don't go negative */
            if (p_lcb->sent_not_acked > num_sent)
                p_lcb->sent_not_acked -= num_sent;
            else
                p_lcb->sent_not_acked = 0;

            l2c_link_check_send_pkts (p_lcb, NULL, NULL);

            /* If we were doing round-robin for low priority links, check 'em */
            if ( (p_lcb->acl_priority == L2CAP_PRIORITY_HIGH)
              && (l2cb.check_round_robin)
              && (l2cb.round_robin_unacked < l2cb.round_robin_quota) )
            {
              l2c_link_check_send_pkts (NULL, NULL, NULL);
            }
#if BLE_INCLUDED == TRUE
            if ((p_lcb->transport == BT_TRANSPORT_LE)
                && (p_lcb->acl_priority == L2CAP_PRIORITY_HIGH)
                && ((l2cb.ble_check_round_robin)
                && (l2cb.ble_round_robin_unacked < l2cb.ble_round_robin_quota)))
            {
              l2c_link_check_send_pkts (NULL, NULL, NULL);
            }
#endif
        }

#if (L2CAP_HCI_FLOW_CONTROL_DEBUG == TRUE)
        if (p_lcb)
        {
#if (BLE_INCLUDED == TRUE)
            if (p_lcb->transport == BT_TRANSPORT_LE)
            {
                L2CAP_TRACE_DEBUG ("TotalWin=%d,LinkUnack(0x%x)=%d,RRCheck=%d,RRUnack=%d",
                    l2cb.controller_le_xmit_window,
                    p_lcb->handle, p_lcb->sent_not_acked,
                    l2cb.ble_check_round_robin, l2cb.ble_round_robin_unacked);
            }
            else
#endif
            {
                L2CAP_TRACE_DEBUG ("TotalWin=%d,LinkUnack(0x%x)=%d,RRCheck=%d,RRUnack=%d",
                    l2cb.controller_xmit_window,
                    p_lcb->handle, p_lcb->sent_not_acked,
                    l2cb.check_round_robin, l2cb.round_robin_unacked);

            }
        }
        else
        {
#if (BLE_INCLUDED == TRUE)
            L2CAP_TRACE_DEBUG ("TotalWin=%d  LE_Win: %d, Handle=0x%x, RRCheck=%d, RRUnack=%d",
                l2cb.controller_xmit_window,
                l2cb.controller_le_xmit_window,
                handle,
                l2cb.ble_check_round_robin, l2cb.ble_round_robin_unacked);
#else
            L2CAP_TRACE_DEBUG ("TotalWin=%d  Handle=0x%x  RRCheck=%d  RRUnack=%d",
                l2cb.controller_xmit_window,
                handle,
                l2cb.check_round_robin, l2cb.round_robin_unacked);
#endif
        }
#endif
    }

#if (defined(HCILP_INCLUDED) && HCILP_INCLUDED == TRUE)
    /* only full stack can enable sleep mode */
    btu_check_bt_sleep ();
#endif
}

/*******************************************************************************
**
** Function         l2c_link_segments_xmitted
**
** Description      This function is called from the HCI Interface when an ACL
**                  data packet segment is transmitted.
**
** Returns          void
**
*******************************************************************************/
void l2c_link_segments_xmitted (BT_HDR *p_msg)
{
    UINT8       *p = (UINT8 *)(p_msg + 1) + p_msg->offset;
    UINT16      handle;
    tL2C_LCB    *p_lcb;

    /* Extract the handle */
    STREAM_TO_UINT16 (handle, p);
    handle   = HCID_GET_HANDLE (handle);

    /* Find the LCB based on the handle */
    if ((p_lcb = l2cu_find_lcb_by_handle (handle)) == NULL)
    {
        L2CAP_TRACE_WARNING ("L2CAP - rcvd segment complete, unknown handle: %d", handle);
        osi_free(p_msg);
        return;
    }

    if (p_lcb->link_state == LST_CONNECTED)
    {
        /* Enqueue the buffer to the head of the transmit queue, and see */
        /* if we can transmit anything more.                             */
        list_prepend(p_lcb->link_xmit_data_q, p_msg);

        p_lcb->partial_segment_being_sent = FALSE;

        l2c_link_check_send_pkts (p_lcb, NULL, NULL);
    }
    else
        osi_free(p_msg);
}