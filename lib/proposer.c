#include <stdlib.h>
#include <stdio.h>
#include <memory.h>
#include <assert.h>

#include "event.h"
#include "evutil.h"

#include "libpaxos.h"
#include "libpaxos_priv.h"
#include "paxos_udp.h"
#include "values_handler.h"


#define PROPOSER_ERROR (-1)

//Lowest instance for which no value has been chosen
static iid_t current_iid = 1;

//Unique identifier of this proposer
short int this_proposer_id = -1;

//Id of the current leader, proposer 0 starts as leader
short int current_leader_id = 0;
#define LEADER_IS_ME (this_proposer_id == current_leader_id)

//UDP socket managers for sending
static udp_send_buffer * to_acceptors;

//UDP socket manager for receiving
static udp_receiver * for_proposer;

//Event: Message received
static struct event proposer_msg_event;

typedef enum instance_status_e {
    empty, 
    p1_pending,
    p1_ready,
    p2_pending, 
    p2_completed
} i_status;

//Structure used to store all info relative to a given instance
typedef struct proposer_instance_info {
    iid_t           iid;
    i_status        status;
    ballot_t        my_ballot;
    size_t          value_size;
    char*           value;
    ballot_t        value_ballot;
    
    unsigned int    promises_bitvector;
    unsigned int    promises_count;

} inst_info;

inst_info proposer_state[PROPOSER_ARRAY_SIZE];
#define GET_PRO_INSTANCE(I) &proposer_state[((I) & (PROPOSER_ARRAY_SIZE-1))]

#define FIRST_BALLOT (MAX_N_OF_PROPOSERS + this_proposer_id)
#define NEXT_BALLOT(B) (B + MAX_N_OF_PROPOSERS)

struct phase1_info {
    unsigned int    pending_count;
    unsigned int    ready_count;
    iid_t           highest_open;
};
struct phase1_info p1_info;

struct phase2_info {
};
struct phase2_info p2_info;

#include "proposer_leader.c"

/*-------------------------------------------------------------------------*/
// Helpers
/*-------------------------------------------------------------------------*/
static void
pro_clear_instance_info(inst_info * ii) {
    ii->iid = 0;
    ii->status = empty;
    ii->my_ballot = 0;
    ii->value_size = 0;
    if(ii->value != NULL) {
        PAX_FREE(ii->value);
    }
    ii->value = NULL;
    ii->value_ballot = 0;
}

static void
pro_save_prepare_ack(inst_info * ii, prepare_ack * pa, short int acceptor_id) {
    
    //Ack from already received!
    if(ii->promises_bitvector & (1<acceptor_id)) {
        LOG(DBG, ("Dropping duplicate promise from:%d, iid:%ld, \n", acceptor_id, ii->iid));
        return;
    }
    
    // promise is new
    ii->promises_bitvector &= (1<acceptor_id);
    ii->promises_count++;
    LOG(DBG, ("Received valid promise from:%d, iid:%ld, \n", acceptor_id, ii->iid));
    
    //Promise contains no value
    if(pa->value_size == 0) {
        LOG(DBG, (" No value in promise\n"));
        return;
    }

    //Promise contains a value
    
    //Our value has same or greater ballot
    if(ii->value_ballot >= pa->value_ballot) {
        //Keep the current value
        LOG(DBG, (" Included value is ignored (cause:value_ballot)\n"));
        return;
    }
    
    //Ballot is greater but the value is actually the same
    if ((ii->value_size == pa->value_size) && 
        (memcmp(ii->value, pa->value, ii->value_size) == 0)) {
        //Just update the value ballot
        LOG(DBG, (" Included value is the same with higher value_ballot\n"));
        ii->value_ballot = pa->value_ballot;
        return;
    }
    
    //Value should replace the one we have (if any)

    //Previous value found
    if(ii->value_size != 0) {
        //Re-enqueue in values list, 
        // will be sent in some future instance
        LOG(DBG, (" Re-enqueuing currently assigned value\n"));
        valhandler_push(ii->value, ii->value_size);
    } else {
        LOG(DBG, (" No previous value was assigned to this instance\n"));
    }

    //Save the received value 
    ii->value = PAX_MALLOC(pa->value_size);
    memcpy(ii->value, pa->value, pa->value_size);
    ii->value_size = pa->value_size;
    ii->value_ballot = pa->value_ballot;
    LOG(DBG, (" Value in promise saved\n"));
}

void 
pro_deliver_callback(char * value, size_t size, iid_t iid, ballot_t ballot, int proposer) {
    LOG(DBG, ("Instance iid:%ld delivered to proposer\n", iid));
    current_iid = iid + 1;

    //TODO clear inst_info
}
/*-------------------------------------------------------------------------*/
// Event handlers
/*-------------------------------------------------------------------------*/

//Returns 1 if the instance became ready, 0 otherwise
static int
handle_prepare_ack(prepare_ack * pa, short int acceptor_id) {
    inst_info * ii = GET_PRO_INSTANCE(pa->iid);
    // If not p1_pending, drop
    if(ii->status != p1_pending) {
        LOG(DBG, ("Promise dropped, iid:%ld not pending\n", pa->iid));
        return 0;
    }
    
    // If not our ballot, drop
    if(pa->ballot != ii->my_ballot) {
        LOG(DBG, ("Promise dropped, iid:%ld not our ballot\n", pa->iid));
        return 0;
    }
    
    //Save the acknowledgement from this acceptor
    //Takes also care of value that may be there
    pro_save_prepare_ack(ii, pa, acceptor_id);
    
    //Not a majority yet for this instance
    if(ii->promises_count < QUORUM) {
        LOG(DBG, ("Not yet a quorum for iid:%ld\n", pa->iid));
        return 0;
    }
    
    //Quorum reached!
    ii->status = p1_ready;
    p1_info.pending_count -= 1;
    p1_info.ready_count += 1;
    //TODO anything else??
    LOG(DBG, ("Quorum for iid:%ld reached\n", pa->iid));
    
    return 1;
}

static void
handle_prepare_ack_batch(prepare_ack_batch* pab) {
    
    //Ignore if not the current leader
    if(!LEADER_IS_ME) {
        return;
    }

    LOG(DBG, ("Got %u promises from acceptor %d\n", 
        pab->count, pab->acceptor_id));
    
    prepare_ack * pa;
    size_t data_offset = 0;
    short int i, ready=0;
    
    for(i = 0; i < pab->count; i++) {
        pa = (prepare_ack *)&pab->data[data_offset];
        ready += handle_prepare_ack(pa, pab->acceptor_id);
        data_offset += PREPARE_ACK_SIZE(pa);
    }
    LOG(DBG, ("%d instances just completed phase 1.\n \
            Status: p1_pending_count:%d, p1_ready_count:%d\n", 
            ready, p1_info.pending_count, p1_info.ready_count));

    
    //Some instance completed phase 1
    if(ready > 0) {
        // try to send a value in phase 2
        leader_open_instances_p2();
    }
}

//This function is invoked when a new message is ready to be read
// from the proposer UDP socket
static void 
pro_handle_newmsg(int sock, short event, void *arg) {
    //Make the compiler happy!
    UNUSED_ARG(sock);
    UNUSED_ARG(event);
    UNUSED_ARG(arg);
    
    assert(sock == for_proposer->sock);
    
    //Read the next message
    int valid = udp_read_next_message(for_proposer);
    if (valid < 0) {
        printf("Dropping invalid proposer message\n");
        return;
    }

    //The message is valid, take the appropriate action
    // based on the type
    paxos_msg * msg = (paxos_msg*) &for_proposer->recv_buffer;
    switch(msg->type) {
        case prepare_acks: {
            handle_prepare_ack_batch((prepare_ack_batch*) msg->data);
        }
        break;

        default: {
            printf("Unknow msg type %d received by proposer\n", msg->type);
        }
    }
}

/*-------------------------------------------------------------------------*/
// Initialization
/*-------------------------------------------------------------------------*/
//Initialize sockets and related events
static int 
init_pro_network() {
    
    // Send buffer for talking to acceptors
    to_acceptors = udp_sendbuf_new(PAXOS_ACCEPTORS_NET);
    if(to_acceptors == NULL) {
        printf("Error creating proposer->acceptors network sender\n");
        return PROPOSER_ERROR;
    }
    
    // Message receive event
    for_proposer = udp_receiver_new(PAXOS_PROPOSERS_NET);
    if (for_proposer == NULL) {
        printf("Error creating proposer network receiver\n");
        return PROPOSER_ERROR;
    }
    event_set(&proposer_msg_event, for_proposer->sock, EV_READ|EV_PERSIST, pro_handle_newmsg, NULL);
    event_add(&proposer_msg_event, NULL);
    
    return 0;
}

//Initialize timers
static int 
init_pro_timers() {
    // TODO remove?
    return 0;
}

//Initialize structures
static int 
init_pro_structs() {
    //Check array size
    if ((PROPOSER_ARRAY_SIZE & (PROPOSER_ARRAY_SIZE -1)) != 0) {
        printf("Error: PROPOSER_ARRAY_SIZE is not a power of 2\n");
        return PROPOSER_ERROR;        
    }
    if (PROPOSER_ARRAY_SIZE <= PROPOSER_PREEXEC_WIN_SIZE) {
        printf("Error: PROPOSER_ARRAY_SIZE = %d is too small\n",
            PROPOSER_ARRAY_SIZE);
        return PROPOSER_ERROR;
    }
    
    // Clear the state array
    memset(proposer_state, 0, (sizeof(inst_info) * PROPOSER_ARRAY_SIZE));
    size_t i;
    for(i = 0; i < PROPOSER_ARRAY_SIZE; i++) {
        pro_clear_instance_info(&proposer_state[i]);
    }
    return 0;

}

//Proposer initialization, this function is invoked by
// the underlying learner after it's normal initialization
static int init_proposer() {
    
    //Add network events and prepare send buffer
    if(init_pro_network() != 0) {
        printf("Proposer network init failed\n");
        return -1;
    }

    //Add additional timers to libevent loop
    if(init_pro_timers() != 0){
        printf("Proposer timers init failed\n");
        return -1;
    }
    
    //Normal proposer initialization, private structures
    if(init_pro_structs() != 0) {
        printf("Proposer structs init failed\n");
        return -1;
    }
    
    //By default, proposer 0 starts as leader, 
    // later on the failure detector may change that
    if(LEADER_IS_ME) {
        if(leader_init() != 0) {
            printf("Proposer Leader init failed\n");
            return -1;
        }
    }
    return 0;
}
/*-------------------------------------------------------------------------*/
// Public functions (see libpaxos.h for more details)
/*-------------------------------------------------------------------------*/


int proposer_init(int proposer_id) {
    
    //Check id validity of proposer_id
    if(proposer_id < 0 || proposer_id >= N_OF_PROPOSERS) {
        printf("Invalid proposer id:%d\n", proposer_id);
        return -1;
    }    
    this_proposer_id = proposer_id;
    LOG(VRB, ("Proposer %d starting...\n", this_proposer_id));
    
    //Starts a learner with a custom init function
    if (learner_init(pro_deliver_callback, init_proposer) != 0) {
        printf("Could not start the learner!\n");
        return -1;
    }

    LOG(VRB, ("Proposer is ready\n"));
    return 0;
}