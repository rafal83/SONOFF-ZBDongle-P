/* SPDX-License-Identifier: MIT */
#include "core_indirect.h"
#include <assert.h>
#include <stdio.h>
static core_indirect_t queue;
static unsigned payloads[100];
static unsigned seed=0x2652;
static unsigned random32(void) { seed^=seed<<13; seed^=seed>>17; seed^=seed<<5; return seed; }
static void broadcast_delivery(void) {
    core_indirect_init(&queue);
    uint32_t mask[4]={1,1,1,UINT32_C(1)<<31};
    core_indirect_token_t b[3]; void *p;
    for (unsigned i=0;i<3;i++) b[i]=core_indirect_broadcast_enqueue(&queue,&payloads[i],mask);
    assert(core_indirect_broadcast_enqueue(&queue,&payloads[4],mask)==CORE_INDIRECT_NONE && queue.count==3);
    assert(core_indirect_broadcast_pending(&queue,b[0])==4);
    core_indirect_token_t u=core_indirect_enqueue(&queue,1,&payloads[5]);
    assert(core_indirect_poll(&queue,1,&p)==u);
    assert(core_indirect_broadcast_poll(&queue,1,0,&p)==CORE_INDIRECT_NONE);
    assert(core_indirect_release(&queue,u,&p));
    assert(core_indirect_broadcast_poll(&queue,1,0,&p)==b[0] && p==&payloads[0]);
    assert(core_indirect_poll(&queue,1,&p)==CORE_INDIRECT_NONE);
    assert(!core_indirect_retry(&queue,b[0]));
    assert(core_indirect_broadcast_confirm(&queue,b[0],false));
    assert(core_indirect_broadcast_pending(&queue,b[0])==4);
    assert(core_indirect_broadcast_poll(&queue,1,0,&p)==b[0]);
    assert(core_indirect_broadcast_confirm(&queue,b[0],true));
    assert(!core_indirect_broadcast_confirm(&queue,b[0],true));
    assert(core_indirect_broadcast_pending(&queue,b[0])==3);
    assert(core_indirect_broadcast_poll(&queue,1,0,&p)==b[1]);
    assert(core_indirect_broadcast_confirm(&queue,b[1],true));
    assert(core_indirect_broadcast_poll(&queue,2,127,&p)==b[0]);
    assert(core_indirect_broadcast_confirm(&queue,b[0],true));
    for (unsigned i=0;i<3;i++) assert(core_indirect_release(&queue,b[i],&p));
    assert(!queue.count && !queue.addresses.count);
}
static void randomized_model(void) {
    struct model { core_indirect_token_t token; unsigned address; bool used, flying; long order; void *payload; } model[24]={0};
    long back=0,front=0;
    core_indirect_init(&queue);
    for (unsigned step=0;step<30000;step++) {
        unsigned op=random32()%4, address=random32()%16, pick=random32()%24;
        unsigned total=0,count=0,empty=24; bool flying=false;
        for (unsigned i=0;i<24;i++) {
            if (!model[i].used) { empty=i; continue; }
            total++;
            if (model[i].address==address) { count++; flying|=model[i].flying; }
        }
        void *p=NULL;
        if (op==0) {
            p=&payloads[step%100];
            core_indirect_token_t id=core_indirect_enqueue(&queue,(uint16_t)address,p);
            if (total==24 || count==3) assert(id==CORE_INDIRECT_NONE);
            else {
                assert(empty<24 && id!=CORE_INDIRECT_NONE);
                model[empty]=(struct model){.token=id,.address=address,.used=true,.order=back++,.payload=p};
            }
        } else if (op==1) {
            unsigned first=24;
            for (unsigned i=0;i<24;i++) if (model[i].used && !model[i].flying && model[i].address==address &&
                (first==24 || model[i].order<model[first].order)) first=i;
            core_indirect_token_t id=core_indirect_poll(&queue,(uint16_t)address,&p);
            if (flying || first==24) assert(id==CORE_INDIRECT_NONE && p==NULL);
            else { assert(id==model[first].token && p==model[first].payload); model[first].flying=true; }
        } else if (op==2 && model[pick].used) {
            bool result=core_indirect_retry(&queue,model[pick].token);
            assert(result==model[pick].flying);
            if (result) { model[pick].flying=false; model[pick].order=--front; }
        } else if (op==3 && model[pick].used) {
            assert(core_indirect_release(&queue,model[pick].token,&p) && p==model[pick].payload);
            model[pick].used=false;
        }
        total=0;
        for (unsigned a=0;a<16;a++) {
            count=0;
            for (unsigned i=0;i<24;i++) if (model[i].used && model[i].address==a) count++;
            assert(count<=3 && core_indirect_count(&queue,(uint16_t)a)==count); total+=count;
        }
        assert(queue.count==total && total<=24);
    }
}
int main(void) {
    core_indirect_init(&queue);
    void *p=(void *)1;
    assert(core_indirect_poll(&queue,0x1234,&p)==CORE_INDIRECT_NONE && p==NULL);
    assert(core_indirect_enqueue(&queue,0xffff,&payloads[0])==CORE_INDIRECT_NONE);
    assert(core_indirect_enqueue(&queue,1,NULL)==CORE_INDIRECT_NONE);
    core_indirect_token_t tokens[24];
    for (unsigned i=0;i<24;i++) tokens[i]=core_indirect_enqueue(&queue,(uint16_t)(i/3),&payloads[i]);
    assert(queue.count==24 && queue.addresses.count==8);
    assert(core_indirect_enqueue(&queue,100,&payloads[30])==CORE_INDIRECT_NONE);
    for (unsigned i=0;i<8;i++) assert(core_indirect_count(&queue,(uint16_t)i)==3);
    assert(core_indirect_poll(&queue,0,&p)==tokens[0] && p==&payloads[0]);
    assert(core_indirect_enqueue(&queue,0,&payloads[31])==CORE_INDIRECT_NONE);
    assert(core_indirect_retry(&queue,tokens[0])); assert(!core_indirect_retry(&queue,tokens[0]));
    assert(core_indirect_poll(&queue,0,&p)==tokens[0] && p==&payloads[0]);
    assert(core_indirect_release(&queue,tokens[0],&p) && p==&payloads[0]);
    assert(!core_indirect_release(&queue,tokens[0],&p) && p==NULL);
    assert(core_indirect_count(&queue,0)==2);
    for (unsigned i=1;i<24;i++) assert(core_indirect_release(&queue,tokens[i],&p) && p==&payloads[i]);
    assert(queue.count==0 && queue.addresses.count==0);
    core_indirect_token_t stale=tokens[23];
    /* Head/middle/tail removal, one in-flight packet per child, retry
     * ordering and complete reuse without lifetime accounting drift. */
    for (unsigned cycle=0;cycle<10000;cycle++) {
        for (unsigned i=0;i<3;i++) tokens[i]=core_indirect_enqueue(&queue,0x1234,&payloads[i]);
        assert(!core_indirect_release(&queue,stale,&p));
        assert(core_indirect_release(&queue,tokens[1],&p));
        assert(core_indirect_poll(&queue,0x1234,&p)==tokens[0]);
        assert(core_indirect_poll(&queue,0x1234,&p)==CORE_INDIRECT_NONE);
        assert(core_indirect_retry(&queue,tokens[0]));
        assert(core_indirect_poll(&queue,0x1234,&p)==tokens[0]);
        assert(core_indirect_release(&queue,tokens[0],&p));
        assert(core_indirect_poll(&queue,0x1234,&p)==tokens[2]);
        assert(core_indirect_retry(&queue,tokens[2]));
        assert(core_indirect_release(&queue,tokens[2],&p));
        assert(!queue.count && !queue.addresses.count);
    }
    randomized_model();
    broadcast_delivery();
    printf("Indirect: 24 total / 3 per child, stale token rejection, 10000 reuse cycles, 30000 reference-model operations; %zu bytes\n",sizeof(queue));
}
