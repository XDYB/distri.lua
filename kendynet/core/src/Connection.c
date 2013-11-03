#include "Connection.h"
#include "link_list.h"
#include <assert.h>
#include "SysTime.h"

#define BUFFER_SIZE 8192

//接收相关函数
static inline void update_next_recv_pos(struct connection *c,int32_t bytestransfer)
{
	uint32_t size;
	do{
		size = c->next_recv_buf->capacity - c->next_recv_pos;
		size = size > (uint32_t)bytestransfer ? (uint32_t)bytestransfer:size;
		c->next_recv_buf->size += size;
		c->next_recv_pos += size;
		bytestransfer -= size;
		if(c->next_recv_pos >= c->next_recv_buf->capacity)
		{
			if(!c->next_recv_buf->next)
				c->next_recv_buf->next = buffer_create_and_acquire(NULL,BUFFER_SIZE);
			c->next_recv_buf = buffer_acquire(c->next_recv_buf,c->next_recv_buf->next);
			c->next_recv_pos = 0;
		}
	}while(bytestransfer);
}

static inline int unpack(struct connection *c)
{
	uint32_t pk_len = 0;
	uint32_t pk_total_size;
	rpacket_t r;
	do{
		if(!c->raw)
		{
			if(c->unpack_size <= sizeof(uint32_t))
				return 1;
			buffer_read(c->unpack_buf,c->unpack_pos,(int8_t*)&pk_len,sizeof(pk_len));
			pk_total_size = pk_len+sizeof(pk_len);
			if(pk_total_size > c->unpack_size)
				return 1;
			r = rpk_create(c->unpack_buf,c->unpack_pos,pk_len,c->raw);
			r->base.tstamp = GetSystemSec();
			//调整unpack_buf和unpack_pos
			do{
				uint32_t size = c->unpack_buf->size - c->unpack_pos;
				size = pk_total_size > size ? size:pk_total_size;
				c->unpack_pos  += size;
				pk_total_size  -= size;
				c->unpack_size -= size;
				if(c->unpack_pos >= c->unpack_buf->capacity)
				{
					/* unpack之前先执行了update_next_recv_pos,在update_next_recv_pos中
					*  如果buffer被填满，会扩展一块新的buffer加入链表中，所以这里的
					*  c->unpack_buf->next不应该会是NULL
					*/
					assert(c->unpack_buf->next);
					c->unpack_pos = 0;
					c->unpack_buf = buffer_acquire(c->unpack_buf,c->unpack_buf->next);
				}
			}while(pk_total_size);
		}
		else
		{
			pk_len = c->unpack_buf->size - c->unpack_pos;
			if(!pk_len)
				return 1;
			r = rpk_create(c->unpack_buf,c->unpack_pos,pk_len,c->raw);
			c->unpack_pos  += pk_len;
			c->unpack_size -= pk_len;
			if(c->unpack_pos >= c->unpack_buf->capacity)
			{
				/* unpack之前先执行了update_next_recv_pos,在update_next_recv_pos中
				*  如果buffer被填满，会扩展一块新的buffer加入链表中，所以这里的
				*  c->unpack_buf->next不应该会是NULL
				*/
				assert(c->unpack_buf->next);
				c->unpack_pos = 0;
				c->unpack_buf = buffer_acquire(c->unpack_buf,c->unpack_buf->next);
			}
		}
		c->_process_packet(c,r);
		rpk_destroy(&r);
		if(c->is_closed) return 0;
	}while(1);
	return 1;
}

//发送相关函数
static inline st_io *prepare_send(struct connection *c)
{
	int32_t i = 0;
	wpacket_t w = (wpacket_t)link_list_head(&c->send_list);
	buffer_t b;
	uint32_t pos;
	st_io *O = NULL;
	uint32_t buffer_size = 0;
	uint32_t size = 0;
	uint32_t send_size_remain = MAX_SEND_SIZE;
	while(w && i < MAX_WBAF && send_size_remain > 0)
	{
		pos = w->base.begin_pos;
		b = w->base.buf;
		buffer_size = w->data_size;
		while(i < MAX_WBAF && b && buffer_size && send_size_remain > 0)
		{
			c->wsendbuf[i].iov_base = b->buf + pos;
			size = b->size - pos;
			size = size > buffer_size ? buffer_size:size;
			size = size > send_size_remain ? send_size_remain:size;
			buffer_size -= size;
			send_size_remain -= size;
			c->wsendbuf[i].iov_len = size;
			++i;
			b = b->next;
			pos = 0;
		}
		if(send_size_remain > 0)
			w = (wpacket_t)w->base.next.next;
	}
	if(i)
	{
		c->send_overlap.m_super.iovec_count = i;
		c->send_overlap.m_super.iovec = c->wsendbuf;
		O = (st_io*)&c->send_overlap;
	}
	return O;

}
static inline int8_t update_send_list(struct connection *c,int32_t bytestransfer)
{
	wpacket_t w;
	uint32_t size;
	while(bytestransfer)
	{
		w = LINK_LIST_POP(wpacket_t,&c->send_list);
		assert(w);
		if((uint32_t)bytestransfer >= w->data_size)
		{
			//一个包发完,检查是否有send_finish需要执行
			if(!LINK_LIST_IS_EMPTY(&c->on_send_finish))
			{
                struct send_finish *sf = (struct send_finish*)link_list_head(&c->on_send_finish);
                if(sf->wpk == w)
                {
                    sf->_send_finish(c,w);
                    link_list_pop(&c->on_send_finish);
                    free(sf);
                }
			}
			bytestransfer -= w->data_size;
			wpk_destroy(&w);
            if(c->is_closed) return 0;
		}
		else
		{
			while(bytestransfer)
			{
				size = w->base.buf->size - w->base.begin_pos;
				size = size > (uint32_t)bytestransfer ? (uint32_t)bytestransfer:size;
				bytestransfer -= size;
				w->base.begin_pos += size;
				w->data_size -= size;
				if(w->base.begin_pos >= w->base.buf->size)
				{
					w->base.begin_pos = 0;
					w->base.buf = buffer_acquire(w->base.buf,w->base.buf->next);
				}
			}
			LINK_LIST_PUSH_FRONT(&c->send_list,w);
		}
	}
	return 1;
}

int32_t send_packet(struct connection *c,wpacket_t w,packet_send_finish pk_send_finish)
{
	if(c->is_closed){
		wpk_destroy(&w);
		return -1;
	}
	st_io *O;
	int8_t start_send = 0;
	if(w){
	    if(LINK_LIST_IS_EMPTY(&c->send_list)) start_send = 1;
		w->base.tstamp = GetSystemMs();
		LINK_LIST_PUSH_BACK(&c->send_list,w);
	}
	if(pk_send_finish){
        struct send_finish *sf = (struct send_finish *)calloc(1,sizeof(*sf));
        sf->wpk = w;
        sf->_send_finish = pk_send_finish;
        LINK_LIST_PUSH_BACK(&c->on_send_finish,sf);
	}
	if(start_send){
		O = prepare_send(c);
		if(O) return Post_Send(c->socket,O);
	}
	return 0;
}

static inline void start_recv(struct connection *c)
{
	c->unpack_buf = buffer_create_and_acquire(NULL,BUFFER_SIZE);
	c->next_recv_buf = buffer_acquire(NULL,c->unpack_buf);
	c->wrecvbuf[0].iov_len = BUFFER_SIZE;
	c->wrecvbuf[0].iov_base = c->next_recv_buf->buf;
	c->recv_overlap.m_super.iovec_count = 1;
	c->recv_overlap.m_super.iovec = c->wrecvbuf;
	c->last_recv = GetSystemMs();
	Post_Recv(c->socket,&c->recv_overlap.m_super);
}

void active_close(struct connection *c)
{
	if(c->is_closed == 0){
		c->is_closed = 1;
		CloseSocket(c->socket);
		printf("active close\n");
		c->_on_disconnect(c,0);
	}
}

void RecvFinish(int32_t bytestransfer,struct connection *c,uint32_t err_code)
{
	uint32_t recv_size;
	uint32_t free_buffer_size;
	buffer_t buf;
	uint32_t pos;
	int32_t i = 0;
	do{
		if(bytestransfer == 0)
			return;
		else if(bytestransfer < 0 && err_code != EAGAIN){
			printf("recv close\n");
			c->is_closed = 1;
            CloseSocket(c->socket);
            c->_on_disconnect(c,err_code);
			return;
		}else if(bytestransfer > 0){
			int32_t total_size = 0;
			do{
				c->last_recv = GetSystemMs();
				update_next_recv_pos(c,bytestransfer);
				c->unpack_size += bytestransfer;
				total_size += bytestransfer;
				if(!unpack(c)) return;
				buf = c->next_recv_buf;
				pos = c->next_recv_pos;
				recv_size = BUFFER_SIZE;
				i = 0;
				do
				{
					free_buffer_size = buf->capacity - pos;
					free_buffer_size = recv_size > free_buffer_size ? free_buffer_size:recv_size;
					c->wrecvbuf[i].iov_len = free_buffer_size;
					c->wrecvbuf[i].iov_base = buf->buf + pos;
					recv_size -= free_buffer_size;
					pos += free_buffer_size;
					if(recv_size && pos >= buf->capacity)
					{
						pos = 0;
						if(!buf->next)
							buf->next = buffer_create_and_acquire(NULL,BUFFER_SIZE);
						buf = buf->next;
					}
					++i;
				}while(recv_size);
				c->recv_overlap.m_super.iovec_count = i;
				c->recv_overlap.m_super.iovec = c->wrecvbuf;
				if(total_size >= BUFFER_SIZE)
				{
					Post_Recv(c->socket,&c->recv_overlap.m_super);
					return;
				}
				else
					bytestransfer = Recv(c->socket,&c->recv_overlap.m_super,&err_code);
			}while(bytestransfer > 0);
		}
	}while(1);
}

void SendFinish(int32_t bytestransfer,struct connection *c,uint32_t err_code)
{
	do{
		if(bytestransfer == 0)
		    return;
		else if(bytestransfer < 0 && err_code != EAGAIN){
			printf("send close\n");
			c->is_closed = 1;
            CloseSocket(c->socket);
            c->_on_disconnect(c,err_code);
			return;
		}else if(bytestransfer > 0)
		{
			do{
				if(!update_send_list(c,bytestransfer)) return;
				st_io *io = prepare_send(c);
				if(!io) return;
				bytestransfer = Send(c->socket,io,&err_code);
			}while(bytestransfer > 0);
		}
	}while(1);
}

void IoFinish(int32_t bytestransfer,st_io *io,uint32_t err_code)
{
    struct OVERLAPCONTEXT *OVERLAP = (struct OVERLAPCONTEXT *)io;
    struct connection *c = OVERLAP->c;
	acquire_conn(c);
    if(io == (st_io*)&c->send_overlap)
        SendFinish(bytestransfer,c,err_code);
    else if(io == (st_io*)&c->recv_overlap)
        RecvFinish(bytestransfer,c,err_code);
	release_conn(c);
}


void connection_destroy(void *arg)
{
	struct connection *c = (struct connection*)arg;
	unregister_timer(con2wheelitem(c));
    wpacket_t w;
    while((w = LINK_LIST_POP(wpacket_t,&c->send_list))!=NULL)
        wpk_destroy(&w);
    struct send_finish *sf;
    while((sf = LINK_LIST_POP(struct send_finish*,&c->on_send_finish))!=NULL)
        free(sf);
    buffer_release(&c->unpack_buf);
    buffer_release(&c->next_recv_buf);
    free(c);
    printf("connection_destroy\n");
}

struct connection *new_conn(SOCK s,uint8_t is_raw)
{
	struct connection *c = calloc(1,sizeof(*c));
	c->socket = s;
	c->recv_overlap.c = c;
	c->send_overlap.c = c;
	c->raw = is_raw;
	c->ref.refcount = 1;
	c->ref.destroyer = connection_destroy;
	return c;
}

void release_conn(struct connection *con){
	ref_decrease((struct refbase *)con);
}

void acquire_conn(struct connection *con){
	ref_increase((struct refbase *)con);
}

int32_t bind2engine(ENGINE e,struct connection *c,
    process_packet _process_packet,on_disconnect _on_disconnect)
{
	c->_process_packet = _process_packet;
	c->_on_disconnect = _on_disconnect;
	start_recv(c);
	return Bind2Engine(e,c->socket,IoFinish,NULL);
}
