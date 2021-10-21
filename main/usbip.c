#include "usbip.h"
#include <stdint.h>
#include <string.h>
#include "lwip/sockets.h"
#include "esp_log.h"

static struct usbip_usb_device udev;

#define __u32 uint32_t
#define __s32 int32_t
//#define __packed __attribute__((packed))

#define USBIP_VERSION 0x111

#define err(...)    ESP_LOGE(TAG, __VA_ARGS__)
#define info(...)   ESP_LOGI(TAG, __VA_ARGS__)
#define dbg(...)    ESP_LOGW(TAG, __VA_ARGS__)

static const char *TAG = "usbip";



/* Defines for op_code status in server/client op_common PDUs */
#define ST_OK	0x00
#define ST_NA	0x01
	/* Device requested for import is not available */
#define ST_DEV_BUSY	0x02
	/* Device requested for import is in error state */
#define ST_DEV_ERR	0x03
#define ST_NODEV	0x04
#define ST_ERROR	0x05

/* ---------------------------------------------------------------------- */
/* Common header for all the kinds of PDUs. */
struct op_common {
	uint16_t version;

#define OP_REQUEST	(0x80 << 8)
#define OP_REPLY	(0x00 << 8)
	uint16_t code;

	/* status codes defined in usbip_common.h */
	uint32_t status; /* op_code status (for reply) */

} __attribute__((packed));

/* ---------------------------------------------------------------------- */
/* Dummy Code */
#define OP_UNSPEC	0x00
#define OP_REQ_UNSPEC	OP_UNSPEC
#define OP_REP_UNSPEC	OP_UNSPEC

/* ---------------------------------------------------------------------- */
/* Retrieve USB device information. (still not used) */
#define OP_DEVINFO	0x02
#define OP_REQ_DEVINFO	(OP_REQUEST | OP_DEVINFO)
#define OP_REP_DEVINFO	(OP_REPLY   | OP_DEVINFO)

struct op_devinfo_request {
	char busid[64];
} __attribute__((packed));

struct op_devinfo_reply {
	struct usbip_usb_device udev;
	struct usbip_usb_interface uinf[];
} __attribute__((packed));

/* ---------------------------------------------------------------------- */
/* Import a remote USB device. */
#define OP_IMPORT	0x03
#define OP_REQ_IMPORT	(OP_REQUEST | OP_IMPORT)
#define OP_REP_IMPORT   (OP_REPLY   | OP_IMPORT)

struct op_import_request {
	char busid[64];
} __attribute__((packed));

struct op_import_reply {
	struct usbip_usb_device udev;
//	struct usbip_usb_interface uinf[];
} __attribute__((packed));

#define PACK_OP_IMPORT_REQUEST(pack, request)  do {\
} while (0)

#define PACK_OP_IMPORT_REPLY(pack, reply)  do {\
	usbip_net_pack_usb_device(pack, &(reply)->udev);\
} while (0)

/* ---------------------------------------------------------------------- */
/* Export a USB device to a remote host. */
#define OP_EXPORT	0x06
#define OP_REQ_EXPORT	(OP_REQUEST | OP_EXPORT)
#define OP_REP_EXPORT	(OP_REPLY   | OP_EXPORT)

struct op_export_request {
	struct usbip_usb_device udev;
} __attribute__((packed));

struct op_export_reply {
	int returncode;
} __attribute__((packed));


#define PACK_OP_EXPORT_REQUEST(pack, request)  do {\
	usbip_net_pack_usb_device(pack, &(request)->udev);\
} while (0)

#define PACK_OP_EXPORT_REPLY(pack, reply)  do {\
} while (0)

/* ---------------------------------------------------------------------- */
/* un-Export a USB device from a remote host. */
#define OP_UNEXPORT	0x07
#define OP_REQ_UNEXPORT	(OP_REQUEST | OP_UNEXPORT)
#define OP_REP_UNEXPORT	(OP_REPLY   | OP_UNEXPORT)

struct op_unexport_request {
	struct usbip_usb_device udev;
} __attribute__((packed));

struct op_unexport_reply {
	int returncode;
} __attribute__((packed));

#define PACK_OP_UNEXPORT_REQUEST(pack, request)  do {\
	usbip_net_pack_usb_device(pack, &(request)->udev);\
} while (0)

#define PACK_OP_UNEXPORT_REPLY(pack, reply)  do {\
} while (0)


/* ---------------------------------------------------------------------- */
/* Retrieve the list of exported USB devices. */
#define OP_DEVLIST	0x05
#define OP_REQ_DEVLIST	(OP_REQUEST | OP_DEVLIST)
#define OP_REP_DEVLIST	(OP_REPLY   | OP_DEVLIST)

struct op_devlist_request {
} __attribute__((packed));

struct op_devlist_reply {
	uint32_t ndev;
	/* followed by reply_extra[] */
} __attribute__((packed));

struct op_devlist_reply_extra {
	struct usbip_usb_device    udev;
	struct usbip_usb_interface uinf[];
} __attribute__((packed));

#define PACK_OP_DEVLIST_REQUEST(pack, request)  do {\
} while (0)

#define PACK_OP_DEVLIST_REPLY(pack, reply)  do {\
	(reply)->ndev = usbip_net_pack_uint32_t(pack, (reply)->ndev);\
} while (0)

/*
 * USB/IP request headers
 *
 * Each request is transferred across the network to its counterpart, which
 * facilitates the normal USB communication. The values contained in the headers
 * are basically the same as in a URB. Currently, four request types are
 * defined:
 *
 *  - USBIP_CMD_SUBMIT: a USB request block, corresponds to usb_submit_urb()
 *    (client to server)
 *
 *  - USBIP_RET_SUBMIT: the result of USBIP_CMD_SUBMIT
 *    (server to client)
 *
 *  - USBIP_CMD_UNLINK: an unlink request of a pending USBIP_CMD_SUBMIT,
 *    corresponds to usb_unlink_urb()
 *    (client to server)
 *
 *  - USBIP_RET_UNLINK: the result of USBIP_CMD_UNLINK
 *    (server to client)
 *
 */
#define USBIP_CMD_SUBMIT	0x0001
#define USBIP_CMD_UNLINK	0x0002
#define USBIP_RET_SUBMIT	0x0003
#define USBIP_RET_UNLINK	0x0004

#define USBIP_DIR_OUT	0x00
#define USBIP_DIR_IN	0x01

/**
 * struct usbip_header_basic - data pertinent to every request
 * @command: the usbip request type
 * @seqnum: sequential number that identifies requests; incremented per
 *	    connection
 * @devid: specifies a remote USB device uniquely instead of busnum and devnum;
 *	   in the stub driver, this value is ((busnum << 16) | devnum)
 * @direction: direction of the transfer
 * @ep: endpoint number
 */
struct usbip_header_basic {
	__u32 command;
	__u32 seqnum;
	__u32 devid;
	__u32 direction;
	__u32 ep;
} __packed;

/**
 * struct usbip_header_cmd_submit - USBIP_CMD_SUBMIT packet header
 * @transfer_flags: URB flags
 * @transfer_buffer_length: the data size for (in) or (out) transfer
 * @start_frame: initial frame for isochronous or interrupt transfers
 * @number_of_packets: number of isochronous packets
 * @interval: maximum time for the request on the server-side host controller
 * @setup: setup data for a control request
 */
struct usbip_header_cmd_submit {
	__u32 transfer_flags;
	__s32 transfer_buffer_length;

	/* it is difficult for usbip to sync frames (reserved only?) */
	__s32 start_frame;
	__s32 number_of_packets;
	__s32 interval;

	unsigned char setup[8];
} __packed;

/**
 * struct usbip_header_ret_submit - USBIP_RET_SUBMIT packet header
 * @status: return status of a non-iso request
 * @actual_length: number of bytes transferred
 * @start_frame: initial frame for isochronous or interrupt transfers
 * @number_of_packets: number of isochronous packets
 * @error_count: number of errors for isochronous transfers
 */
struct usbip_header_ret_submit {
	__s32 status;
	__s32 actual_length;
	__s32 start_frame;
	__s32 number_of_packets;
	__s32 error_count;
} __packed;

/**
 * struct usbip_header_cmd_unlink - USBIP_CMD_UNLINK packet header
 * @seqnum: the URB seqnum to unlink
 */
struct usbip_header_cmd_unlink {
	__u32 seqnum;
} __packed;

/**
 * struct usbip_header_ret_unlink - USBIP_RET_UNLINK packet header
 * @status: return status of the request
 */
struct usbip_header_ret_unlink {
	__s32 status;
} __packed;

/**
 * struct usbip_header - common header for all usbip packets
 * @base: the basic header
 * @u: packet type dependent header
 */
struct usbip_header {
	struct usbip_header_basic base;

	union {
		struct usbip_header_cmd_submit	cmd_submit;
		struct usbip_header_ret_submit	ret_submit;
		struct usbip_header_cmd_unlink	cmd_unlink;
		struct usbip_header_ret_unlink	ret_unlink;
	} u;
} __packed;

uint32_t usbip_net_pack_uint32_t(int pack, uint32_t num)
{
	uint32_t i;

	if (pack)
		i = htonl(num);
	else
		i = ntohl(num);

	return i;
}

uint16_t usbip_net_pack_uint16_t(int pack, uint16_t num)
{
	uint16_t i;

	if (pack)
		i = htons(num);
	else
		i = ntohs(num);

	return i;
}

void usbip_net_pack_usb_device(int pack, struct usbip_usb_device *udev)
{
	udev->busnum = usbip_net_pack_uint32_t(pack, udev->busnum);
	udev->devnum = usbip_net_pack_uint32_t(pack, udev->devnum);
	udev->speed = usbip_net_pack_uint32_t(pack, udev->speed);

	udev->idVendor = usbip_net_pack_uint16_t(pack, udev->idVendor);
	udev->idProduct = usbip_net_pack_uint16_t(pack, udev->idProduct);
	udev->bcdDevice = usbip_net_pack_uint16_t(pack, udev->bcdDevice);
}

void usbip_net_pack_usb_interface(int pack __attribute__((unused)),
				  struct usbip_usb_interface *udev
				  __attribute__((unused)))
{
	/* uint8_t members need nothing */
}


static inline void usbip_net_pack_op_common(int pack,
					    struct op_common *op_common)
{
	op_common->version = usbip_net_pack_uint16_t(pack, op_common->version);
	op_common->code = usbip_net_pack_uint16_t(pack, op_common->code);
	op_common->status = usbip_net_pack_uint32_t(pack, op_common->status);
}

static ssize_t usbip_net_xmit(int sockfd, void *buff, size_t bufflen,
			      int sending)
{
	ssize_t nbytes;
	ssize_t total = 0;

	if (!bufflen)
		return 0;

	do {
		if (sending)
			nbytes = send(sockfd, buff, bufflen, 0);
		else
			nbytes = recv(sockfd, buff, bufflen, MSG_WAITALL);

		if (nbytes <= 0)
			return -1;

		buff	 = (void *)((intptr_t) buff + nbytes);
		bufflen	-= nbytes;
		total	+= nbytes;

	} while (bufflen > 0);

	return total;
}

ssize_t usbip_net_recv(int sockfd, void *buff, size_t bufflen)
{
	return usbip_net_xmit(sockfd, buff, bufflen, 0);
}

ssize_t usbip_net_send(int sockfd, void *buff, size_t bufflen)
{
	return usbip_net_xmit(sockfd, buff, bufflen, 1);
}

int usbip_net_send_op_common(int sockfd, uint32_t code, uint32_t status)
{
	struct op_common op_common;
	int rc;

	memset(&op_common, 0, sizeof(op_common));

	op_common.version = USBIP_VERSION;
	op_common.code    = code;
	op_common.status  = status;

	usbip_net_pack_op_common(1, &op_common);

	rc = usbip_net_send(sockfd, &op_common, sizeof(op_common));
	if (rc < 0) {
		dbg("usbip_net_send failed: %d", rc);
		return -1;
	}

	return 0;
}

static int send_reply_devlist(int connfd)
{
	//struct usbip_exported_device *edev;
	struct usbip_usb_device pdu_udev;
	struct usbip_usb_interface pdu_uinf = {1,2,3,4};
	struct op_devlist_reply reply;
	//struct list_head *j;
	int rc, i;

	/*
	 * Exclude devices that are already exported to a client from
	 * the exportable device list to avoid:
	 *	- import requests for devices that are exported only to
	 *	  fail the request.
	 *	- revealing devices that are imported by a client to
	 *	  another client.
	 */

	reply.ndev = 1;
	/* number of exported devices */
	// list_for_each(j, &driver->edev_list) {
	// 	edev = list_entry(j, struct usbip_exported_device, node);
	// 	if (edev->status != SDEV_ST_USED)
	// 		reply.ndev += 1;
	// }
	info("exportable devices: %d", reply.ndev);

	rc = usbip_net_send_op_common(connfd, OP_REP_DEVLIST, ST_OK);
	if (rc < 0) {
		dbg("usbip_net_send_op_common failed: %#0x", OP_REP_DEVLIST);
		return -1;
	}
	PACK_OP_DEVLIST_REPLY(1, &reply);

	rc = usbip_net_send(connfd, &reply, sizeof(reply));
	if (rc < 0) {
		dbg("usbip_net_send failed: %#0x", OP_REP_DEVLIST);
		return -1;
	}

	// list_for_each(j, &driver->edev_list) {
	// 	edev = list_entry(j, struct usbip_exported_device, node);
	// 	if (edev->status == SDEV_ST_USED)
	// 		continue;

	//	dump_usb_device(&edev->udev);
		memcpy(&pdu_udev, &udev, sizeof(pdu_udev));
		usbip_net_pack_usb_device(1, &pdu_udev);

		rc = usbip_net_send(connfd, &pdu_udev, sizeof(pdu_udev));
		if (rc < 0) {
			dbg("usbip_net_send failed: pdu_udev");
			return -1;
		}

	// 	for (i = 0; i < edev->udev.bNumInterfaces; i++) {
	// //		dump_usb_interface(&edev->uinf[i]);
	// 		memcpy(&pdu_uinf, &edev->uinf[i], sizeof(pdu_uinf));
	 		usbip_net_pack_usb_interface(1, &pdu_uinf);

	 		rc = usbip_net_send(connfd, &pdu_uinf,
	 				sizeof(pdu_uinf));
			if (rc < 0) {
				err("usbip_net_send failed: pdu_uinf");
				return -1;
	 		}
            dbg("complete send %d", rc);
//		}
//	}

	return 0;
}


static int recv_request_devlist(int connfd)
{
	struct op_devlist_request req;
	int rc;

	memset(&req, 0, sizeof(req));

	rc = usbip_net_recv(connfd, &req, sizeof(req));
	if (rc < 0) {
		dbg("usbip_net_recv failed: devlist request");
		return -1;
	}

	rc = send_reply_devlist(connfd);
	if (rc < 0) {
		dbg("send_reply_devlist failed");
		return -1;
	}

	return 0;
}


int usbip_net_recv_op_common(int sockfd, uint16_t *code, int *status)
{
	struct op_common op_common;
	int rc;

	memset(&op_common, 0, sizeof(op_common));

	rc = usbip_net_recv(sockfd, &op_common, sizeof(op_common));
	if (rc < 0) {
		dbg("usbip_net_recv failed: %d", rc);
		goto err;
	}

	usbip_net_pack_op_common(0, &op_common);

	if (op_common.version != USBIP_VERSION) {
		err("USBIP Kernel and tool version mismatch: %d %d:",
		    op_common.version, USBIP_VERSION);
		goto err;
	}

	switch (*code) {
	case OP_UNSPEC:
		break;
	default:
		if (op_common.code != *code) {
			dbg("unexpected pdu %#0x for %#0x", op_common.code,
			    *code);
			/* return error status */
			*status = ST_ERROR;
			goto err;
		}
	}

	*status = op_common.status;

	if (op_common.status != ST_OK) {
		dbg("request failed at peer: %d", op_common.status);
		goto err;
	}

	*code = op_common.code;

	return 0;
err:
	return -1;
}

static int recv_pdu(int connfd)
{
	uint16_t code = OP_UNSPEC;
	int ret;
	int status;

	ret = usbip_net_recv_op_common(connfd, &code, &status);
	if (ret < 0) {
		dbg("could not receive opcode: %#0x", code);
		return -1;
	}

	//ret = usbip_refresh_device_list(driver);
	if (ret < 0) {
		dbg("could not refresh device list: %d", ret);
		return -1;
	}

	info("received request: %#0x(%d)", code, connfd);
	switch (code) {
	case OP_REQ_DEVLIST:
		ret = recv_request_devlist(connfd);
		break;
	case OP_REQ_IMPORT:
	//	ret = recv_request_import(connfd);
		break;
	case OP_REQ_DEVINFO:
	default:
		err("received an unknown opcode: %#0x", code);
		ret = -1;
	}

	if (ret == 0)
		info("request %#0x(%d): complete", code, connfd);
	else
		info("request %#0x(%d): failed", code, connfd);

	return ret;
}


void usbip_add_device(const struct usbip_usb_device * dev) {
    udev = *dev;
}


void do_tcp_task(const int sock)
{
    int len;
   // char rx_buffer[128];

    do {
        len = recv_pdu(sock);
        if (len < 0) {
            ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
        } else if (len == 0) {
            ESP_LOGW(TAG, "good");
        } else {
          //  rx_buffer[len] = 0; // Null-terminate whatever is received and treat it like a string
         //   ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);

            // send() can return less bytes than supplied length.
            // Walk-around for robust implementation.
            // int to_write = len;
            // while (to_write > 0) {
            //     int written = send(sock, rx_buffer + (len - to_write), to_write, 0);
            //     if (written < 0) {
            //         ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
            //     }
            //     to_write -= written;
            // }
        }
    } while (len >= 0);
}