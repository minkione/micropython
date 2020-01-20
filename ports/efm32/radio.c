/*
 * Interface to the EFM32 radio module in IEEE 802.15.4 mode
 */
#include <stdio.h>
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"
#include "py/objarray.h"
#include "em_gpio.h"
#include "em_cmu.h"
#include "em_core.h"
#include "radio.h"
#include "zrepl.h"

#include "rail/rail.h"
#include "rail/pa.h"
#include "rail/ieee802154/rail_ieee802154.h"


typedef enum {
    RADIO_UNINIT,
    RADIO_INITING,
    RADIO_IDLE,
    RADIO_TX,
    RADIO_RX,
    RADIO_CALIBRATION
} siliconlabs_modem_state_t;

static siliconlabs_modem_state_t radio_state = RADIO_UNINIT;

static int channel = 11;
static volatile RAIL_Handle_t rail = NULL;
static void rail_callback_events(RAIL_Handle_t rail, RAIL_Events_t events);

// can't be const since the buffer is used for writes
static RAIL_Config_t rail_config = {
	.eventsCallback		= rail_callback_events,
	.protocol		= NULL, // must be NULL for ieee802.15.4
	.scheduler		= NULL, // not multi-protocol
	.buffer			= {}, // must be zero
};

static const RAIL_DataConfig_t rail_data_config = {
	.txSource = TX_PACKET_DATA,
	.rxSource = RX_PACKET_DATA,
	.txMethod = PACKET_MODE,
	.rxMethod = PACKET_MODE,
};


static const RAIL_IEEE802154_Config_t ieee802154_config = {
	.promiscuousMode	= false,
	.isPanCoordinator	= false,
	.framesMask		= RAIL_IEEE802154_ACCEPT_STANDARD_FRAMES,
	.ackConfig = {
		.enable			= true,
		.ackTimeout		= 54 * 16, // 54 symbols * 16 us/symbol = 864 usec
		.rxTransitions = {
			.success = RAIL_RF_STATE_RX, // go to Tx to send the ACK
			.error = RAIL_RF_STATE_RX, // ignored
		},
		.txTransitions = {
			.success = RAIL_RF_STATE_RX, // go to Rx for receiving the ACK
			.error = RAIL_RF_STATE_RX, // ignored
		},
	},
	.timings = {
		.idleToRx		= 100,
		.idleToTx		= 100,
		.rxToTx			= 192, // 12 symbols * 16 us/symbol
		.txToRx			= 192 - 10, // slightly lower to make sure we get to RX in time
		.rxSearchTimeout	= 0, // not used
		.txToRxSearchTimeout	= 0, // not used
	},
	.addresses		= NULL, // will be set by explicit calls
};

static const RAIL_TxPowerConfig_t paInit2p4 = {
	.mode			= RAIL_TX_POWER_MODE_2P4_HP,
	.voltage		= 1800,
	.rampTime		= 10,
};

static const RAIL_CsmaConfig_t csma_config = RAIL_CSMA_CONFIG_802_15_4_2003_2p4_GHz_OQPSK_CSMA;

uint8_t radio_mac_address[8];

/*
 * Called when radio calibration is required
 */
void RAILCb_CalNeeded()
{
	printf("calibrateRadio\n");
	//calibrateRadio = true;
}

static void rail_callback_rfready(RAIL_Handle_t rail)
{
	radio_state = RADIO_IDLE;
	radio_tx_pending = 0;
}

#define MAX_PKTS 8
static volatile unsigned rx_buffer_write;
static volatile unsigned rx_buffer_read;
static uint8_t rx_buffers[MAX_PKTS][MAC_PACKET_MAX_LENGTH + MAC_PACKET_INFO_LENGTH];
static uint8_t rx_buffer_copy[MAC_PACKET_MAX_LENGTH + MAC_PACKET_INFO_LENGTH];

uint8_t radio_tx_buffer[MAC_PACKET_MAX_LENGTH+1];
volatile int radio_tx_pending;

static void process_packet(RAIL_Handle_t rail)
{
	RAIL_RxPacketInfo_t info;
	RAIL_RxPacketHandle_t handle = RAIL_GetRxPacketInfo(rail, RAIL_RX_PACKET_HANDLE_NEWEST, &info);
	if (info.packetStatus != RAIL_RX_PACKET_READY_SUCCESS)
		return;

	// deal with acks early
/*
	uint8_t header[4];
	RAIL_PeekRxPacket(rail, handle, header, 4, 0);
	if (header[0] == 5
	&&  header[3] == current_tx_sequence
	&& waiting_for_ack)
	{
		RAIL_CancelAutoAck(rail);
		waiting_for_ack = false;
		last_ack_pending_bit = (header[1] & (1 << 4)) != 0;
		printf("got ack\n");
	} else {
*/
 {
		RAIL_RxPacketDetails_t details;
		details.timeReceived.timePosition = RAIL_PACKET_TIME_DEFAULT;
		details.timeReceived.totalPacketBytes = 0;
		RAIL_GetRxPacketDetails(rail, handle, &details);

		unsigned write_index = rx_buffer_write;
		uint8_t * rx_buffer = rx_buffers[write_index];
		RAIL_CopyRxPacket(rx_buffer, &info); // puts the length in byte 0

		// allow the zrepl to process this message
		// length is in the first byte and includes
		// an extra two bytes at the end that we ignore
		zrepl_recv(rx_buffer+1, *rx_buffer - 2);

		if (write_index == MAX_PKTS - 1)
			rx_buffer_write = 0;
		else
			rx_buffer_write = write_index + 1;

/*
		// cancel the ACK if the sender did not request one
		// buffer[0] == length
		// buffer[1] == frame_type[0:2], security[3], frame_pending[4], ack_req[5], intrapan[6]
		// buffer[2] == destmode[2:3], version[4:5], srcmode[6:7]
		if ((rx_buffer[1] & (1 << 5)) == 0)
			RAIL_CancelAutoAck(rail);
*/

#if 0
		printf("rx %2d bytes lqi=%d rssi=%d:", info.packetBytes, details.lqi, details.rssi);

		// skip the length byte
		for(int i = 1 ; i < info.packetBytes ; i++)
		{
			printf(" %02x", rx_buffer[i]);
		}
		printf("\n");
#endif
	}

	RAIL_ReleaseRxPacket(rail, handle);
}


/*
 * Callbank from the radio interrupt when there is an event.
 * rx 00000000:00018030
 */

static void rail_callback_events(RAIL_Handle_t rail, RAIL_Events_t events)
{
/*
	// ignore certain bit patterns unless other things are set
	events &= ~( 0
		| RAIL_EVENT_RX_PREAMBLE_LOST
		| RAIL_EVENT_RX_PREAMBLE_DETECT
		| RAIL_EVENT_RX_TIMING_LOST
		| RAIL_EVENT_RX_TIMING_DETECT
	);

	if(events == 0)
		return;

	printf("rx %08x:%08x\n", (unsigned int)(events >> 32), (unsigned int)(events >> 0));
*/

	if (events & RAIL_EVENT_RSSI_AVERAGE_DONE)
	{
		printf("rssi %d\n", RAIL_GetAverageRssi(rail));
	}

	if (events & RAIL_EVENT_RX_ACK_TIMEOUT)
	{
		//
	}

	if (events & RAIL_EVENT_RX_PACKET_RECEIVED)
	{
		process_packet(rail);
	}

	if (events & RAIL_EVENT_IEEE802154_DATA_REQUEST_COMMAND)
	{
           /*
            * Indicates a Data Request is being received when using IEEE 802.15.4
            * functionality. This occurs when the command byte of an incoming frame is
            * for a data request, which requests an ACK. This callback is called before
            * the packet is fully received to allow the node to have more time to decide
            * whether to set the frame pending in the outgoing ACK. This event only ever
            * occurs if the RAIL IEEE 802.15.4 functionality is enabled.
            *
            * Call \ref RAIL_IEEE802154_GetAddress to get the source address of the
            * packet.
            */
	}

	if (events & RAIL_EVENTS_TX_COMPLETION)
	{
		// they are done with our packet, either for good or worse
		//printf("TX done!\n");
		radio_tx_pending = 0;
	}

	if (events & RAIL_EVENT_CAL_NEEDED)
	{
		// we should flag that a calibration is needed
		// does this cancel any transmits?
		radio_tx_pending = 0;
	}

	// lots of other events that we don't handle
}


mp_obj_t radio_init(void)
{
	// do not re-init
	if (radio_state != RADIO_UNINIT)
		return mp_const_none;

	if(0)
	printf("%s: mac %08x:%08x\n", __func__, (unsigned int) DEVINFO->UNIQUEH, (unsigned int) DEVINFO->UNIQUEL);

	rail = RAIL_Init(&rail_config, rail_callback_rfready);
	RAIL_ConfigData(rail, &rail_data_config);
	RAIL_ConfigCal(rail, RAIL_CAL_ALL);
	RAIL_IEEE802154_Config2p4GHzRadio(rail);
	RAIL_IEEE802154_Init(rail, &ieee802154_config);
	RAIL_ConfigEvents(rail, RAIL_EVENTS_ALL, 0
		| RAIL_EVENT_RSSI_AVERAGE_DONE
		| RAIL_EVENT_RX_ACK_TIMEOUT
		| RAIL_EVENT_RX_PACKET_RECEIVED
		| RAIL_EVENT_IEEE802154_DATA_REQUEST_COMMAND
		| RAIL_EVENT_TX_PACKET_SENT
		| RAIL_EVENT_CAL_NEEDED
	);

	RAIL_ConfigTxPower(rail, &paInit2p4);
	RAIL_SetTxPower(rail, 255); // max

	// use the device unique id as the mac for network index 0
	memcpy(&radio_mac_address[0], (const void*)&DEVINFO->UNIQUEL, 4);
	memcpy(&radio_mac_address[4], (const void*)&DEVINFO->UNIQUEH, 4);
	RAIL_IEEE802154_SetLongAddress(rail, radio_mac_address, 0);

	// start the radio
	RAIL_Idle(rail, RAIL_IDLE_FORCE_SHUTDOWN_CLEAR_FLAGS, true);
	radio_state = RADIO_RX;
	RAIL_StartRx(rail, channel, NULL);

	return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_0(radio_init_obj, radio_init);


/*
 * Copy the rx buffer into the passed in argument.
 */
static mp_obj_t radio_rxbytes_get(void)
{
	if (radio_state == RADIO_UNINIT)
		radio_init();

	if (rx_buffer_write == rx_buffer_read)
		return mp_const_none;

	static mp_obj_t rx_buffer_bytearray;
	if (!rx_buffer_bytearray)
		rx_buffer_bytearray = mp_obj_new_bytearray_by_ref(sizeof(rx_buffer_copy), rx_buffer_copy);

	// resize the buffer for the return code and copy into it
	mp_obj_array_t * buf = MP_OBJ_TO_PTR(rx_buffer_bytearray);
	unsigned read_index = rx_buffer_read;
	uint8_t * const rx_buffer = rx_buffers[read_index];

	CORE_ATOMIC_IRQ_DISABLE();
	buf->len = rx_buffer[0] - 2;
	if (buf->len > MAC_PACKET_MAX_LENGTH)
	{
		// discard this packet? should signal something
		buf->len = MAC_PACKET_MAX_LENGTH;
	}

	memcpy(rx_buffer_copy, rx_buffer+1, buf->len);
	if (read_index == MAX_PKTS - 1)
		rx_buffer_read = 0;
	else
		rx_buffer_read = read_index + 1;

	CORE_ATOMIC_IRQ_ENABLE();

	return rx_buffer_bytearray;
}

MP_DEFINE_CONST_FUN_OBJ_0(radio_rxbytes_obj, radio_rxbytes_get);


// length of message should be in radio_tx_buffer[0]
// returns 0 if ok, -1 if not
int radio_send_tx_buffer(void)
{
	CORE_ATOMIC_IRQ_DISABLE();

	radio_tx_pending = 1;
	radio_state = RADIO_TX;
	RAIL_Idle(rail, RAIL_IDLE_ABORT, true);

	const uint8_t len = radio_tx_buffer[0] - 2;
	RAIL_SetTxFifo(rail, radio_tx_buffer, len + 1, len + 1);
	static const RAIL_TxOptions_t txOpt = RAIL_TX_OPTIONS_DEFAULT;

/*
	// check if we're waiting for an ack, but not yet implemented
	if (radio_tx_buffer[1+1] & (1 << 5))
		txOpt |= RAIL_TX_OPTION_WAIT_FOR_ACK;
*/

	// start the transmit, we hope!
	int rc = RAIL_StartCcaCsmaTx(rail, channel, txOpt, &csma_config, NULL);

	// if it failed to start, unset the pending flag
	if (rc != 0)
		radio_tx_pending = 0;

	CORE_ATOMIC_IRQ_ENABLE();
	return rc;
}


/*
 * Send a byte buffer to the radio.
 */
static mp_obj_t radio_txbytes(mp_obj_t buf_obj)
{
	if (radio_state == RADIO_UNINIT)
		radio_init();

	// should block instead?
	//mp_raise_ValueError("tx pending");
	unsigned timeout_counter = 0;
	while (radio_tx_pending)
	{
		if (timeout_counter++ > 100000)
			mp_raise_ValueError("tx timeout");
	}
		

	mp_buffer_info_t buf;
	mp_get_buffer_raise(buf_obj, &buf, MP_BUFFER_READ);
	unsigned len = buf.len;
	if (len > MAC_PACKET_MAX_LENGTH)
		mp_raise_ValueError("tx length too long");
	radio_tx_buffer[0] = 2 + (uint8_t) buf.len; // include hardware FCS
	memcpy(radio_tx_buffer+1, buf.buf, len);

	int rc = radio_send_tx_buffer();
	if (rc != 0)
		mp_raise_ValueError("tx failed");

	return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_1(radio_txbytes_obj, radio_txbytes);


static mp_obj_t radio_mac(void)
{
	if (radio_state == RADIO_UNINIT)
		radio_init();

	static mp_obj_t mac_bytes;
	if (!mac_bytes)
		mac_bytes = mp_obj_new_bytes(radio_mac_address, sizeof(radio_mac_address));
	return mac_bytes;
}

MP_DEFINE_CONST_FUN_OBJ_0(radio_mac_obj, radio_mac);


static mp_obj_t radio_promiscuous(mp_obj_t value_obj)
{
	if (radio_state == RADIO_UNINIT)
		radio_init();

	int status = mp_obj_int_get_checked(value_obj);

	printf("radio: %s promiscuous mode\n", status ? "enabling" : "disabling");
	RAIL_IEEE802154_SetPromiscuousMode(rail, status);

	return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_1(radio_promiscuous_obj, radio_promiscuous);


static mp_obj_t radio_address(mp_obj_t short_addr_obj, mp_obj_t pan_id_obj)
{
	if (radio_state == RADIO_UNINIT)
		radio_init();

 	unsigned short_addr = mp_obj_int_get_checked(short_addr_obj);
 	unsigned pan_id = mp_obj_int_get_checked(pan_id_obj);

	printf("radio: addr %04x/%04x\n", pan_id, short_addr);
	RAIL_IEEE802154_SetPanId(rail, pan_id, 0);
	RAIL_IEEE802154_SetShortAddress(rail, short_addr, 0);

	return mp_const_none;
}

MP_DEFINE_CONST_FUN_OBJ_2(radio_address_obj, radio_address);

STATIC const mp_map_elem_t radio_globals_table[] = {
    { MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_radio) },
    { MP_OBJ_NEW_QSTR(MP_QSTR_init), (mp_obj_t) &radio_init_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_promiscuous), (mp_obj_t) &radio_promiscuous_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_address), (mp_obj_t) &radio_address_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_mac), (mp_obj_t) &radio_mac_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_rx), (mp_obj_t) &radio_rxbytes_obj },
    { MP_OBJ_NEW_QSTR(MP_QSTR_tx), (mp_obj_t) &radio_txbytes_obj },
};

STATIC MP_DEFINE_CONST_DICT (
    mp_module_radio_globals,
    radio_globals_table
);

const mp_obj_module_t mp_module_radio = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_radio_globals,
};

MP_REGISTER_MODULE(MP_QSTR_Radio, mp_module_radio, 1);
