/* 
 * modified from http://www.digitalvampire.org/rdma-tutorial-2007/notes.pdf
 * 
 * build:
 * gcc -o client rdma_write_client.c -lrdmacm -libverbs
 * 
 * usage:
 * ./client <servername or ip> <val1> <val2>
 * 
 * connect to server, send two integers, and waits for server to send back the sum.
 */ 

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <byteswap.h>
#include <rdma/rdma_cma.h>

enum { 
    RESOLVE_TIMEOUT_MS = 5000, 
}; 
struct pdata { 
    uint64_t	buf_va; 
    uint32_t	buf_rkey;
};

int prepare_send_notify_after_rdma_write(struct rdma_cm_id *cm_id, struct ibv_pd *pd)
{
	struct ibv_sge					sge; 
   	struct ibv_send_wr				send_wr = { }; 
   	struct ibv_send_wr 				*bad_send_wr; 

	uint8_t *buf = calloc(1, sizeof(uint8_t));
	struct ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(uint8_t), IBV_ACCESS_LOCAL_WRITE); 
	if (!mr) 
		return 1;
	
	sge.addr = (uintptr_t)buf; 
    sge.length = sizeof(uint8_t); 
    sge.lkey = mr->lkey;
    
	memset(&send_wr, 0, sizeof(send_wr));
	send_wr.wr_id = 2;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;

    if (ibv_post_send(cm_id->qp,&send_wr,&bad_send_wr)) 
        return 1;

	return 0;
}

int main(int argc, char *argv[]) 
{
   	struct pdata					server_pdata;

   	struct rdma_event_channel		*cm_channel; 
   	struct rdma_cm_id				*cm_id; 
   	struct rdma_cm_event			*event;  
   	struct rdma_conn_param			conn_param = { };

   	struct ibv_pd					*pd; 
   	struct ibv_comp_channel			*comp_chan; 
   	struct ibv_cq					*cq; 
   	struct ibv_cq					*evt_cq; 
   	struct ibv_mr					*mr; 
   	struct ibv_qp_init_attr			qp_attr = { }; 
   	struct ibv_sge					sge; 
   	struct ibv_send_wr				send_wr = { }; 
   	struct ibv_send_wr 				*bad_send_wr; 
   	struct ibv_recv_wr				recv_wr = { }; 
   	struct ibv_recv_wr				*bad_recv_wr; 
   	struct ibv_wc					wc; 
   	void							*cq_context; 
   	struct addrinfo					*res, *t; 
   	struct addrinfo					hints = { 
   		.ai_family    = AF_INET,
   		.ai_socktype  = SOCK_STREAM
   	};
	int								n; 
	uint32_t						*buf; 
	int								err;
     
    /* We use rdmacm lib to establish rdma connection and ibv lib to write, read, send, receive data here. */

    /* In RDMA programming, transmission is a "asychronize" procedure, all the "events" were generated on NIC. 
     * Programmer should "get" those event and than ack and process them. 
     */

    /* In rdmacm lib, each event will generated by NIC and we should "get" these events from event channel, 
     * so we should create an event channel first. 
     */
	cm_channel = rdma_create_event_channel(); 
	if (!cm_channel)  
		return 1; 

    /* Like socket fd in socket porgramming, we need to acquire a rdmacm id.
     */
	err = rdma_create_id(cm_channel, &cm_id, NULL, RDMA_PS_TCP);
	if (err)  
		return err;

    /* Note: port 20000 doesn't equal to the socket port in TCP/IP, 
     * in RoCEv2, all of the packets use port 4791,
     * port 20000 here indicates a higher level abstraction port
     */
	n = getaddrinfo(argv[1], "20000", &hints, &res);
	if (n < 0)  
		return 1;

	/* Resolve addr. */
	err = rdma_resolve_addr(cm_id, NULL, res->ai_addr, RESOLVE_TIMEOUT_MS);
	if (err)
		return err;
    /* We need to "get" rdmacm event to acquire event occured on NIC. */
	err = rdma_get_cm_event(cm_channel, &event);
	if (err)
		return err;
	if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED)
		return 1;
    /* Each rdmacm event should be acked. */
	rdma_ack_cm_event(event);

	err = rdma_resolve_route(cm_id, RESOLVE_TIMEOUT_MS);
	if (err)
		return err;
	err = rdma_get_cm_event(cm_channel, &event);
	if (err)
		return err;
	if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED)
		return 1; 
	rdma_ack_cm_event(event);

	/* Allocate protection domain, each pd can be used to create queue pair, 
     * register memory regien, etc.
     * Each pd is a protection of a group of objects, 
     * it means you can't use these objects between different pd.
     */
	pd = ibv_alloc_pd(cm_id->verbs); 
	if (!pd) 
		return 1;
    /* A completion event channel like rdma_create_event_channel in libibverbs */
	comp_chan = ibv_create_comp_channel(cm_id->verbs);
	if (!comp_chan) 
		return 1;
    /* create a completion queue, a cq contains a completion work request.
     * All the events about NIC, transmission will be in the cq 
     * Since libibverbs is thread-safe, use multiple cqs to 1 or many completion channels is avaliable.
     */
	cq = ibv_create_cq(cm_id->verbs,2,NULL,comp_chan,0); 
	if (!cq) 
		return 1;
    /* Requests create compiletion notification when any work completion is add to the cq,
     * therefore work completion can be "get" by using ibv_get_cq_event() 
     */
	if (ibv_req_notify_cq(cq,0))
		return 1;

	buf = calloc(2,sizeof(uint32_t)); 
	if (!buf) 
		return 1;
    /* register a memory region with a specific pd */
	mr = ibv_reg_mr(pd, buf,2 * sizeof(uint32_t), IBV_ACCESS_LOCAL_WRITE); 
	if (!mr) 
		return 1;

	qp_attr.cap.max_send_wr = 4; 
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_wr = 1; 
	qp_attr.cap.max_recv_sge = 1; 

	qp_attr.send_cq        = cq;
	qp_attr.recv_cq        = cq;
	qp_attr.qp_type        = IBV_QPT_RC;
    /* create a queue pair, a qp is for post send/receive.
     * If pd is NULL, rdma_create_qp will use default pd on RDMA device
     */
	err = rdma_create_qp(cm_id,pd,&qp_attr);
	if (err)
		return err;

	conn_param.initiator_depth = 1;
	conn_param.retry_count     = 7;

	err = rdma_connect(cm_id,&conn_param);
	if (err)
		return err;

	err = rdma_get_cm_event(cm_channel,&event);
	if (err)
		return err;

	if (event->event != RDMA_CM_EVENT_ESTABLISHED)
		return 1;
    /* event->param.conn.private_data includes the memory info at server */
	memcpy(&server_pdata,event->param.conn.private_data,sizeof(server_pdata));
	rdma_ack_cm_event(event);

	/* We prepare ibv_post_recv() first */

	sge.addr = (uintptr_t)buf; 
	sge.length = sizeof(uint32_t);
	sge.lkey = mr->lkey;

    /* wr_id is used to identify the recv data when get ibv event */
	recv_wr.wr_id =     0;                
	recv_wr.sg_list =   &sge;
	recv_wr.num_sge =   1;

	if (ibv_post_recv(cm_id->qp,&recv_wr,&bad_recv_wr))
		return 1;

	buf[0] = strtoul(argv[2],NULL,0);
	buf[1] = strtoul(argv[3],NULL,0);
	buf[0] = htonl(buf[0]);
	buf[1] = htonl(buf[1]);

	sge.addr 					  = (uintptr_t)buf; 
	sge.length                    = sizeof(buf);
	sge.lkey                      = mr->lkey;

	send_wr.wr_id                 = 1;
	send_wr.opcode                = IBV_WR_RDMA_WRITE;
    /* set IBV_SEND_SIGNALED flag will cause an ibv event recv at sender when data transmit from memory to NIC */
	send_wr.send_flags            = IBV_SEND_SIGNALED;
	send_wr.sg_list               = &sge;
	send_wr.num_sge               = 1;
	send_wr.wr.rdma.rkey          = ntohl(server_pdata.buf_rkey);
	send_wr.wr.rdma.remote_addr   = bswap_64(server_pdata.buf_va);

	if (ibv_post_send(cm_id->qp,&send_wr,&bad_send_wr))
		return 1;

	int end_loop = 0;
	while (!end_loop) {
		if (ibv_get_cq_event(comp_chan,&evt_cq,&cq_context))
			return 1;
		if (ibv_req_notify_cq(cq,0))
			return 1;
		if (ibv_poll_cq(cq,1,&wc) != 1)
			return 1;
		if (wc.status != IBV_WC_SUCCESS)
			return 1;
		switch (wc.wr_id) {
		case 0:
			printf("server ans : %d\n", ntohl(buf[0]));
			end_loop = 1;
			break;
		case 1:
			/* due to server side doesn't know when the IBV_WR_RDMA_WRITE is done,
			 * we need to send a notification to tell server side the IBV_WR_RDMA_WRITE is alreay sent 
			 */
			if (prepare_send_notify_after_rdma_write(cm_id, pd))
				return 1;
			break;
		default:
			end_loop = 1;
			break;
		}
    }
	ibv_ack_cq_events(cq,2);
	rdma_disconnect(cm_id);
	err = rdma_get_cm_event(cm_channel,&event);
	if (err)
		return err;

	rdma_ack_cm_event(event);
	rdma_destroy_qp(cm_id);
	ibv_dereg_mr(mr);
	free(buf);
	err = rdma_destroy_id(cm_id);
	if (err)  {
		perror("rdma_destroy_id");
		return err;
	}
	rdma_destroy_event_channel(cm_channel);
    return 0;
}