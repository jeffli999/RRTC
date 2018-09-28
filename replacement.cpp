#include <stdio.h>
#include <iostream>
#include <stdlib.h>
#include <stdint.h>
#include <map>
#include <vector>
#include <mhash.h>
#include <assert.h>
#include <time.h>
#include <fstream>
#include <sstream>
#include <set>
#include <bitset>
using namespace std;

int  CACHE_LINE_SIZE;	// cache每行元素个数
#define CACHE_LINE_NUM 1	// cache行数

#define FIFO 1       //先进先出（First In，First Out）
#define RANDOM 2
#define LRU 3        //Least recently used，最近最少使用
#define LFU 4        //Least Frequently Used
#define ADT 5
#define ADT2 6
#define BTE 7        //benefit to elephant,即RRTC
#define MICE 11
#define ELEPHANT 12
#define RABBIT 13
#define Null -1

FILE *fp;
FILE *fp_stat;

int cur_policy;	// 当前的替换策略

int num = 0;

struct _5tuple // 流的五元组
{
	u_int src_ip;
	u_int dst_ip;
	u_short src_port;
	u_short dst_port;
	u_char proto;
	
	// 运算符重载，用于map插入比较
	bool operator < (const struct _5tuple &a) const
 	{
 		if (src_ip < a.src_ip)
 		{
 			return true;
 		}
 		else if (src_ip == a.src_ip && dst_ip < a.dst_ip)
 		{
 			return true;
 		}
 		else if (src_ip == a.src_ip && dst_ip == a.dst_ip && src_port < a.src_port)
 		{
 			return true;
 		}
 		else if (src_ip == a.src_ip && dst_ip == a.dst_ip && src_port == a.src_port && dst_port < a.dst_port)
 		{
 			return true;
 		}
 		else if (src_ip == a.src_ip && dst_ip == a.dst_ip && src_port == a.src_port && dst_port == a.dst_port && proto < a.proto)
 		{
 			return true;
 		}
 		else
 		{
 			return false;
 		}
  	}
  	bool operator == (const struct _5tuple &a) const
  	{
  		if (src_ip == a.src_ip && dst_ip == a.dst_ip && src_port == a.src_port && dst_port == a.dst_port && proto == a.proto)
  		{
  			return true;
  		}
  		else
  		{
  			return false;
  		}
  	}
};

struct cache_node	// 每个cache结点
{
	struct _5tuple tuple;	// 五元组
	u_char valid;	// cache结点是否有效，如一开始cold start时，all valid = 0
	int index_i;	// cache结点所在cache矩阵中的位置
	int index_j;
};

struct evict_info
{
	int mice;
	int rabbit;
	int elephant;
};

struct cache_line_info	// cache行的信息
{
	int valid_node_num;	// 该cache行中有效结点的数目（用于cold start完成的条件判断）
	int evict_times;
	int lived_time;   //该cache行的存活时间
};

struct final_stat
{
	int times[32];
};

struct flow_info	// 每个流的信息，保存在flow_table结构中（正反两个方向的流合并成同一个流，这里的流更恰当说应该为连接）
{
	u_int len;	// 流的长度（数据包个数）
	u_int evict_times;	// 流发生cache替换的次数
	u_int hit_times;	// 流命中的总次数
	u_int cur_hit_times;    //流在cache中的命中次数
	u_int live_times;       //流在cache中的存活时间
	u_int clock_flag;             //时钟标志
	int flow_type;	// 流的类型 ELEPHANT MICE RABBIT
	int cache_line_evict_times;
};


map<struct _5tuple, struct flow_info> pre_flow_table;	// 预处理流表结构，用于统计流的类型

vector<vector<cache_node> > cache;
map<struct _5tuple, struct flow_info> flow_table;	// 流表结构
struct cache_line_info cache_line[CACHE_LINE_NUM];	// 每行cache的统计信息结构
map<u_int, struct evict_info> evict_stat;
struct final_stat burst_stat;

void dump_flow_stats();

/*
 * ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
 *
 * my new policy, use real tarffic characteristics
 */
struct nf_counter{
	int Fsum;
	int Csum;
};
nf_counter NF_Counter;
map<struct _5tuple, int> NF_Table;  //(f, c)
map<struct _5tuple, int> HH_Table; //(f, c)
const int sampleSize = 2000;  //师兄设置为1000
const int hhSize = 500;
const int maxC = 50;
int updateTime = 0;
int x = 0; //标志位

/*
 * map不是严格O（1）
 */
int check( _5tuple f){
	int x = 0;
	static uint64_t slamp = 1;
	//检查标志位
	if(HH_Table.count(f)){ //在HH_Table中命中
		x = 1;
		//if(HH_Table[f] < maxC)
			HH_Table[f] += 1;
	}else{
		x = 0;
		NF_Counter.Csum += 1;
		if(NF_Table.count(f)){//在NF_Table中命中
			//if(NF_Table[f] < maxC)
				NF_Table[f] += 1;
		}else{
			NF_Table[f] = 1;
			NF_Counter.Fsum += 1;
		}
	}
	//更新HH_Table和NF_Table
	if(slamp % sampleSize == 0){
		int d =  (NF_Counter.Fsum == 0 ? 0 : NF_Counter.Csum / NF_Counter.Fsum); //计算衰减因子
		//cout<<NF_Counter.Csum<<'\t'<<NF_Counter.Fsum<<endl;
		for(auto iterHH = HH_Table.begin(); iterHH != HH_Table.end(); ){//更新HH_Table,移除c = 0的项
			//cout << "HH_Table heavy hitters: "<<iterHH->first.src_ip<<"\n";
			if (iterHH->second <= 2*d ) {
				HH_Table.erase(iterHH++);
			}
			else {
				iterHH->second -= 2*d;
				iterHH++;
			}
			//if(iterHH->second <= 2 * d){
				//HH_Table.erase(iterHH++);
			//}else{
				//iterHH->second -= 2 * d;
				//iterHH++;
			//}
		}
		for(auto iterNF = NF_Table.begin(); iterNF != NF_Table.end(); iterNF++){//更新HH_Table，插入新的项
			if(HH_Table.size() >= hhSize) break;
			//if (iterNF->second > d) HH_Table[iterNF->first] = iterNF->second - d;
			if(iterNF->second > d) HH_Table[iterNF->first] = iterNF->second - d;
		}
		NF_Table.clear(); //清空NF_Table
		NF_Counter.Csum = NF_Counter.Fsum = 0; //清空NF_Counter
	}

	slamp++;
	return x;
}
/*
 * ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
 */

/*
 *
 * 直接hash，严格O(1)
 */
vector<uint> NF_Hash(2000, 0);  //(f, c)
map<uint, uint> HH_Hash; //(f, c)
u_int tuple_hash(struct _5tuple *tuple, u_int number);
int check_hash( _5tuple f){
	int x = 0;
	static uint64_t slamp = 1;
	//检查标志位
	uint idx = tuple_hash(&f, 800);
	if(HH_Hash.count(idx)){ //在HH_Hash中命中
		//cout << "HH_Hash heavy hitters: "<<f.src_ip <<"\t" << f.dst_ip << "\t" << f.src_port << "\t" << f.dst_port << "\t" << f.proto << "\n";
		x = 1;
		//if(HH_Table[f] < maxC)
		HH_Hash[idx] += 1;
	}else{
		x = 0;
		NF_Counter.Csum += 1;
		if(NF_Hash[idx]){//在NF_Table中命中
			//if(NF_Table[f] < maxC)
			NF_Hash[idx] += 1;
		}else{
			NF_Hash[idx] = 1;
			NF_Counter.Fsum += 1;
		}
	}
	//更新HH_Table和NF_Table
	if(slamp % sampleSize == 0){
		int d =  (NF_Counter.Fsum == 0 ? 0 : NF_Counter.Csum / NF_Counter.Fsum); //计算衰减因子
		//cout<<NF_Counter.Csum<<'\t'<<NF_Counter.Fsum<<endl;
		for(auto iterHH = HH_Hash.begin(); iterHH != HH_Hash.end(); ){//更新HH_Table,移除c = 0的项
			//cout << "HH_Hash heavy hitters: " << iterHH->first.src_ip << "\t" << iterHH->first.dst_ip << "\t" << iterHH->first.src_port << "\t" << iterHH->first.dst_port << "\t" << iterHH->first.proto << "\n";
			//cout << "HH_Hash heavy hitters: " << iterHH->first <<"\t"<< iterHH->second << "\n";
			if(iterHH->second <= 2*d ){
				HH_Hash.erase(iterHH++);
			}else{
				iterHH->second -= 2*d;
				iterHH++;
			}
		}
		for(int i = 0; i < NF_Hash.size(); i++){//更新HH_Table，插入新的项
			if(HH_Hash.size() >= hhSize) break;
			if(NF_Hash[i] > d) HH_Hash[i] = NF_Hash[i] - d;
		}
		NF_Hash.clear(); //清空NF_Table
		NF_Hash.resize(2000, 0);
		NF_Counter.Csum = NF_Counter.Fsum = 0; //清空NF_Counter
	}

	slamp++;
	return x;
}
/*
 * -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
 */

int loc_clock[CACHE_LINE_NUM];
void cache_init(int policy)
{
	cur_policy = policy; //init the policy
	int i, j;
	for (i = 0; i < CACHE_LINE_NUM; i++)
	{
		for (j = 0; j < CACHE_LINE_SIZE; j++)
		{
			cache[i][j].valid = 0;
			cache[i][j].index_i = i;
			cache[i][j].index_j = j;
		}
		cache_line[i].lived_time = 0;
	}
}

int get_random(int max) // return a number ranging from 0 - max-1
{
	return rand() % max;
}

void reverse(struct _5tuple *t)
{
	if (t->src_ip < t->dst_ip)	// 满足条件，则调换源和目的信息，将两个不同方向的流合并为一个
    {
		t->src_ip ^= t->dst_ip;
		t->dst_ip ^= t->src_ip;
		t->src_ip ^= t->dst_ip;
		t->src_port ^= t->dst_port;
		t->dst_port ^= t->src_port;
		t->src_port ^= t->dst_port;
	}
}

u_int tuple_hash(struct _5tuple *tuple, u_int number)	// 采用md5算法将五元组哈希到cache行
{
	MHASH td;
	u_char hash[16];
	td = mhash_init(MHASH_MD5);
	if (td == MHASH_FAILED)
	{
		exit(1);
	}
	mhash(td, tuple, sizeof(struct _5tuple));
	mhash_deinit(td, hash);
	u_int *integer = (u_int *)hash;
	return (*integer) % number;
}

int flow_table_query(struct _5tuple *tuple)	// 查询流表
{
	map<struct _5tuple, struct flow_info>::iterator iter = flow_table.find(*tuple);
	if (iter == flow_table.end())
	{
		return 0;
	}
	return 1;
}

int pre_flow_table_query(struct _5tuple *tuple)	// 查询流表
{
	map<struct _5tuple, struct flow_info>::iterator iter = pre_flow_table.find(*tuple);
	if (iter == pre_flow_table.end())
	{
		return 0;
	}
	return 1;
}

int evict_query(u_int evict_times)	// 查询流表
{
	map<u_int, struct evict_info>::iterator iter = evict_stat.find(evict_times);
	if (iter == evict_stat.end())
	{
		return 0;
	}
	return 1;
}

void rotate(int loc_from, int loc_to, u_int addr)
{
	int i;
	struct cache_node temp;
	temp.tuple.src_ip = cache[addr][loc_from].tuple.src_ip;
	temp.tuple.dst_ip = cache[addr][loc_from].tuple.dst_ip;
	temp.tuple.src_port = cache[addr][loc_from].tuple.src_port;
	temp.tuple.dst_port = cache[addr][loc_from].tuple.dst_port;
	temp.tuple.proto = cache[addr][loc_from].tuple.proto;
	temp.valid = cache[addr][loc_from].valid;

	for (i = loc_from; i > loc_to; i--)
	{
		cache[addr][i].valid = cache[addr][i-1].valid;
		cache[addr][i].tuple.src_ip = cache[addr][i-1].tuple.src_ip;
		cache[addr][i].tuple.dst_ip = cache[addr][i-1].tuple.dst_ip;
		cache[addr][i].tuple.src_port = cache[addr][i-1].tuple.src_port;
		cache[addr][i].tuple.dst_port = cache[addr][i-1].tuple.dst_port;
		cache[addr][i].tuple.proto = cache[addr][i-1].tuple.proto;
	}

	cache[addr][loc_to].valid = temp.valid;
	cache[addr][loc_to].tuple.src_ip = temp.tuple.src_ip;
	cache[addr][loc_to].tuple.dst_ip = temp.tuple.dst_ip;
	cache[addr][loc_to].tuple.src_port = temp.tuple.src_port;
	cache[addr][loc_to].tuple.dst_port = temp.tuple.dst_port;
	cache[addr][loc_to].tuple.proto = temp.tuple.proto;
}

int cache_query(struct _5tuple *tuple)	// 查询cache
{
	u_int cache_addr = tuple_hash(tuple, CACHE_LINE_NUM);
	int hit = 0;
	int i;
	int hit_loc = 0;
	for (i = 0; i < CACHE_LINE_SIZE; i++)
	{
		if (cache[cache_addr][i].valid && cache[cache_addr][i].tuple == *tuple)	// 元素有效且五元组相同，命中
		{
			hit = 1;
			hit_loc = i;
			flow_table[*tuple].hit_times++;	// 该流的命中次数增加
			flow_table[*tuple].cur_hit_times++;
			break;
		}
	}
	if (flow_table_query(tuple))
	{
		flow_table[*tuple].len++;	// 如果在流表中能找到，则增加流中包的个数
	}
	if (hit == 1)
	{
		// hit adjustment
		switch (cur_policy)
		{
			case FIFO:
				break;
			case RANDOM:
				break;
			case LFU:
				break;
			case LRU:
				updateTime++;
				rotate(hit_loc, 0, cache_addr);	// 命中的交换到cache首部
				break;
			case ADT:
				break;
			case ADT2:
				if(flow_table[*tuple].evict_times <=5 );
				else if(flow_table[*tuple].evict_times <=100)
				{
					rotate(hit_loc, 4, cache_addr);
				}
				else
				{
					rotate(hit_loc, 0, cache_addr);
				}
				break;
			case BTE:
				if(x == 1){
					rotate(hit_loc, 0, cache_addr);	// 命中的交换到cache首部
					updateTime++;
				}else{
					//rotate(hit_loc, 0, cache_addr);
				}
				//rotate(hit_loc, 0, cache_addr);	// 命中的交换到cache首部
				break;
			default:
				break;
		}
	}
	else
	{
		cache_line[cache_addr].evict_times++;
	}

	return hit;
}

void flow_table_insert(struct _5tuple *tuple)	// 在流表中插入一个新结点
{
	struct flow_info *info = (struct flow_info *)malloc(sizeof(struct flow_info));
	info->len = 0;	// 初始化
	info->evict_times = 0;
	info->hit_times = 0;
	info->cur_hit_times = 0;
	info->live_times = 0;
	info->clock_flag = 0;
	info->cache_line_evict_times = 0;

	pair<map<struct _5tuple, struct flow_info>::iterator, bool> inserted;
	inserted = flow_table.insert(map<struct _5tuple, struct flow_info>::value_type(*tuple, *info));
	if (true != inserted.second)
	{
		printf("insert failed!\n");	// 插入失败
	}
	else
	{
		flow_table[*tuple].len++;	// 插入成功，增加该流包个数
	}
}

void pre_flow_table_insert(struct _5tuple *tuple)	// 在流表中插入一个新结点
{
	struct flow_info *info = (struct flow_info *)malloc(sizeof(struct flow_info));
	info->len = 0;	// 初始化
	info->evict_times = 0;
	info->hit_times = 0;
	info->cur_hit_times = 0;
	info->live_times = 0;
	info->clock_flag = 0;
	info->cache_line_evict_times = 0;

	pair<map<struct _5tuple, struct flow_info>::iterator, bool> inserted;
	inserted = pre_flow_table.insert(map<struct _5tuple, struct flow_info>::value_type(*tuple, *info));
	if (true != inserted.second)
	{
		printf("insert failed!\n");	// 插入失败
	}
	else
	{
		pre_flow_table[*tuple].len++;	// 插入成功，增加该流包个数
	}
}

void evict_insert(u_int evict_times)
{
	struct evict_info *info = (struct evict_info *)malloc(sizeof(struct evict_info));
	info->mice = 0;
	info->rabbit = 0;
	info->elephant = 0;

	pair<map<u_int, struct evict_info>::iterator, bool> inserted;
	inserted = evict_stat.insert(map<u_int, struct evict_info>::value_type(evict_times, *info));
	if (true != inserted.second)
	{
		printf("insert failed!\n");	// 插入失败
	}
}

void flow_table_update(struct _5tuple *tuple)	// 每次从cache中被替换出的元素被送入流表，并更新流表信息，此处更新换入换出次数
{
	if (flow_table_query(tuple))
	{
		flow_table[*tuple].evict_times++;
	}
	else
	{
		printf("updating a flow not existed!\n");
	}
}

void pre_flow_table_update(struct _5tuple *tuple)
{
	if (pre_flow_table_query(tuple))
	{
		pre_flow_table[*tuple].len++;
	}
	else
	{
		printf("updating a flow not existed!\n");
	}
}

void replace(int loc, struct _5tuple *swap_in, struct _5tuple *swap_out, u_int addr)	// 将loc位置的元素赋给swap_out换出，换入swap_in
{
	struct cache_node *node_cursor = &cache[addr][loc];
	swap_out->src_ip = node_cursor->tuple.src_ip;
	swap_out->dst_ip = node_cursor->tuple.dst_ip;
	swap_out->src_port = node_cursor->tuple.src_port;
	swap_out->dst_port = node_cursor->tuple.dst_port;
	swap_out->proto = node_cursor->tuple.proto;
	node_cursor->tuple.src_ip = swap_in->src_ip;
	node_cursor->tuple.dst_ip = swap_in->dst_ip;
	node_cursor->tuple.src_port = swap_in->src_port;
	node_cursor->tuple.dst_port = swap_in->dst_port;
	node_cursor->tuple.proto = swap_in->proto;
}

void insert_blank(struct _5tuple *swap_in, u_int addr)	// cache行cold start时，逐渐将其填满
{
	int i;
	struct cache_node *node_cursor;
	for (i = 0; i < CACHE_LINE_SIZE ; i++)
	{
		node_cursor = &cache[addr][i];
		if (!node_cursor->valid)
		{
			break;
		}
	}
	node_cursor->tuple.src_ip = swap_in->src_ip;
	node_cursor->tuple.dst_ip = swap_in->dst_ip;
	node_cursor->tuple.src_port = swap_in->src_port;
	node_cursor->tuple.dst_port = swap_in->dst_port;
	node_cursor->tuple.proto = swap_in->proto;
	node_cursor->valid = 1;
	cache_line[addr].valid_node_num++;
}

void delete_entity(u_int addr)  //将cache行清空
{
	int i;
	struct cache_node *node_cursor;
	for (i = 0; i < CACHE_LINE_SIZE; i++)
	{
		node_cursor = &cache[addr][i];
		if (node_cursor->valid)
		{
			node_cursor->valid = 0;
			flow_table_update(&node_cursor->tuple);
		}
	}
	cache_line[addr].valid_node_num = 0;
}
void insert(int loc, struct _5tuple *swap_in, struct _5tuple *swap_out, u_int addr)
{
	struct cache_node *node_cursor = &cache[addr][CACHE_LINE_SIZE-1];
	swap_out->src_ip = node_cursor->tuple.src_ip;
	swap_out->dst_ip = node_cursor->tuple.dst_ip;
	swap_out->src_port = node_cursor->tuple.src_port;
	swap_out->dst_port = node_cursor->tuple.dst_port;
	swap_out->proto = node_cursor->tuple.proto;
	int i;
	for (i = CACHE_LINE_SIZE - 1; i > loc; i--)
	{
		cache[addr][i].valid = cache[addr][i-1].valid;
		cache[addr][i].tuple.src_ip = cache[addr][i-1].tuple.src_ip;
		cache[addr][i].tuple.dst_ip = cache[addr][i-1].tuple.dst_ip;
		cache[addr][i].tuple.src_port = cache[addr][i-1].tuple.src_port;
		cache[addr][i].tuple.dst_port = cache[addr][i-1].tuple.dst_port;
		cache[addr][i].tuple.proto = cache[addr][i-1].tuple.proto;
	}

	cache[addr][loc].valid = 1;
	cache[addr][loc].tuple.src_ip = swap_in->src_ip;
	cache[addr][loc].tuple.dst_ip = swap_in->dst_ip;
	cache[addr][loc].tuple.src_port = swap_in->src_port;
	cache[addr][loc].tuple.dst_port = swap_in->dst_port;
	cache[addr][loc].tuple.proto = swap_in->proto;

	if (cache_line[addr].valid_node_num != CACHE_LINE_SIZE)
	{
		cache_line[addr].valid_node_num++;
	}
}


int cache_swap(struct _5tuple *swap_in, struct _5tuple *swap_out)	// cache替换
{
	u_int cache_addr = tuple_hash(swap_in, CACHE_LINE_NUM);
	int evict = (cache_line[cache_addr].valid_node_num == CACHE_LINE_SIZE); // 如果cold start阶段已经结束，则将evict标记置1
	int min_evict_times,max_evict_times;
	int min_cur_hit_times;
	min_evict_times = max_evict_times = flow_table[cache[cache_addr][0].tuple].evict_times;
	min_cur_hit_times = flow_table[cache[cache_addr][0].tuple].cur_hit_times;
	int i, vol = 0, vol2 = 0;
	if(evict == 0)
	{
		insert_blank(swap_in, cache_addr);
		return evict;
	}
	switch (cur_policy)
	{
		case FIFO:
			insert(0, swap_in, swap_out, cache_addr);
			break;
		case RANDOM:
			replace(get_random(CACHE_LINE_SIZE), swap_in, swap_out, cache_addr);	// 随机替换
			break;
		case LFU:
			for(i = 1; i < CACHE_LINE_SIZE; i++)
			{
				if(flow_table[cache[cache_addr][i].tuple].cur_hit_times < min_cur_hit_times)
				{
					min_cur_hit_times = flow_table[cache[cache_addr][i].tuple].cur_hit_times;
					vol = i;
				}

			}
			replace(vol, swap_in, swap_out, cache_addr);
			break;
		case LRU:
			insert(0, swap_in, swap_out, cache_addr);
			break;
		case ADT:
			cache_line[cache_addr].lived_time++;
			//方案1
			/*if(cache_line[cache_addr].lived_time == 1024)
			{
				cache_line[cache_addr].lived_time = 0;
				delete_entity(cache_addr);
				insert_blank(swap_in, cache_addr);
				return 0;
			}*/
			//方案2
			for(i = 1; i < CACHE_LINE_SIZE-2; i++)
			{
				flow_table[cache[cache_addr][i].tuple].live_times++;
				if(flow_table[cache[cache_addr][i].tuple].live_times == 256)
				{
					flow_table[cache[cache_addr][i].tuple].live_times = 0;
					replace(i, swap_in, swap_out, cache_addr);
					return evict;
				}
				if(flow_table[cache[cache_addr][i].tuple].evict_times < min_evict_times)
				{
					min_evict_times = flow_table[cache[cache_addr][i].tuple].evict_times;
					vol = i;
				}

			}
			replace(vol, swap_in, swap_out, cache_addr);
			break;
		case ADT2:
			if(flow_table[*swap_in].evict_times <=5 )
			{
				replace(CACHE_LINE_SIZE - 1, swap_in, swap_out, cache_addr);
			}
			else if(flow_table[*swap_in].evict_times <= 100)
			{
				insert(4, swap_in, swap_out, cache_addr);
			}
			else
			{
				flow_table[*swap_in].live_times = 1;
				insert(0, swap_in, swap_out, cache_addr);
			}
			if(flow_table[*swap_out].live_times > 0)
				flow_table[*swap_out].live_times--;
			if(flow_table[*swap_out].live_times > 0)
			{
				*swap_in = *swap_out;
				insert(0, swap_in, swap_out, cache_addr);
			}
			break;
		case BTE:
			if(x == 1){
				insert(0, swap_in, swap_out, cache_addr);
			}else{
				insert(CACHE_LINE_SIZE - 1, swap_in, swap_out, cache_addr);
			}
			break;
		default:
			break;
	}
	flow_table[*swap_out].cur_hit_times = 0;
	return evict;
}

void stat()	// 统计
{
	u_int figure1_packet[9];
	u_int figure1_flow[9];
	u_int figure1_evict[9];
	int i;
	for (i = 0; i < 9; i++)
	{
		figure1_packet[i] = 0;
		figure1_flow[i] = 0;
		figure1_evict[i] = 0;
	}
	u_int evict_sum = 0;
	u_int hit_sum = 0;
	u_int len_sum = 0;
	u_int a = 0, b = 0, c = 0;
	// traverse flow table
	map<struct _5tuple, struct flow_info>::iterator it;
	for (it = flow_table.begin(); it != flow_table.end(); it++)
	{
		evict_sum += it->second.evict_times;
		hit_sum += it->second.hit_times;
		len_sum += it->second.len;

		if (it->second.len == 1)
		{
			figure1_packet[0] += it->second.len;
			figure1_flow[0]++;
			figure1_evict[0] += it->second.evict_times;
		}
		else if (it->second.len <= 5)
		{
			figure1_packet[1] += it->second.len;
			figure1_flow[1]++;
			figure1_evict[1] += it->second.evict_times;
		}
		else if (it->second.len <= 20)
		{
			figure1_packet[2] += it->second.len;
			figure1_flow[2]++;
			figure1_evict[2] += it->second.evict_times;
		}
		else if (it->second.len <= 50)
		{
			figure1_packet[3] += it->second.len;
			figure1_flow[3]++;
			figure1_evict[3] += it->second.evict_times;
		}
		else if (it->second.len <= 100)
		{
			figure1_packet[4] += it->second.len;
			figure1_flow[4]++;
			figure1_evict[4] += it->second.evict_times;
		}
		else if (it->second.len <= 200)
		{
			figure1_packet[5] += it->second.len;
			figure1_flow[5]++;
			figure1_evict[5] += it->second.evict_times;
		}
		else if (it->second.len <= 500)
		{
			figure1_packet[6] += it->second.len;
			figure1_flow[6]++;
			figure1_evict[6] += it->second.evict_times;
		}
		else if (it->second.len <= 1000)
		{
			figure1_packet[7] += it->second.len;
			figure1_flow[7]++;
			figure1_evict[7] += it->second.evict_times;
		}
		else
		{
			figure1_packet[8] += it->second.len;
			figure1_flow[8]++;
			figure1_evict[8] += it->second.evict_times;
		}

		if (it->second.len < 20)
		{
			a += it->second.evict_times;  //a小鼠流驱逐次数
		}
		/*else if (it->second.len <= 20)
		{
			b += it->second.evict_times;
		}*/
		else
		{
			b += it->second.evict_times;  //b大象流驱逐次数
		}

		if (!evict_query(it->second.evict_times))
		{
			evict_insert(it->second.evict_times);
		}
		if (evict_query(it->second.evict_times))
		{
			if (it->second.len < 4)
			{
				evict_stat[it->second.evict_times].mice += it->second.evict_times;
			}
			else if (it->second.len < 20)
			{
				evict_stat[it->second.evict_times].rabbit += it->second.evict_times;
			}
			else
			{
				evict_stat[it->second.evict_times].elephant += it->second.evict_times;
			}
		}
		else
		{
			printf("some error!\n");
		}
		/*if (it->second.len > 0)
		{
			fprintf(fp_stat, "%u, %u\n", it->second.len, it->second.evict_times);
		}*/

	}
	printf("cache size num = %d\n", CACHE_LINE_SIZE);
	printf("updateTime = %d\n", updateTime);
	printf("hitTime = %d\n", hit_sum);    //缓存命中次数
	printf("hit rate = %f\n", (double)hit_sum/len_sum);
	/*
	printf("packet_range        flow_sum       packet_sum     total_evict_times\n");
	printf("         [1]        %u             %u             %u\n", figure1_flow[0], figure1_packet[0], figure1_evict[0]);
	printf("       [2,5]        %u             %u             %u\n", figure1_flow[1], figure1_packet[1], figure1_evict[1]);
	printf("      [6,20]        %u             %u             %u\n", figure1_flow[2], figure1_packet[2], figure1_evict[2]);
	printf("     [21,50]        %u             %u             %u\n", figure1_flow[3], figure1_packet[3], figure1_evict[3]);
	printf("    [51,100]        %u             %u             %u\n", figure1_flow[4], figure1_packet[4], figure1_evict[4]);
	printf("   [101,200]        %u             %u             %u\n", figure1_flow[5], figure1_packet[5], figure1_evict[5]);
	printf("   [201,500]        %u             %u             %u\n", figure1_flow[6], figure1_packet[6], figure1_evict[6]);
	printf("  [501,1000]        %u             %u             %u\n", figure1_flow[7], figure1_packet[7], figure1_evict[7]);
	printf("    [1000,+]        %u             %u             %u\n", figure1_flow[8], figure1_packet[8], figure1_evict[8]);
	*/

	//printf("a = %u, b = %u\n", a, b);

	/*map<u_int, struct evict_info>::iterator it2;
	for (it2 = evict_stat.begin(); it2 != evict_stat.end(); it2++)
	{
		fprintf(fp_stat, "%u, %d, %d, %d\n", it2->first, it2->second.mice, it2->second.rabbit, it2->second.elephant);
	}*/

	/*for (i = 0; i < 32; i++)
	{
		fprintf(fp_stat, "[%d-%d), %d\n", i*4, (i+1)*4, burst_stat.times[i]);
	}*/

}

/*
 * 构造trace
 */
void rebuild(){
	ofstream out;
	string filename = "rebuild";
	out.open(filename.c_str());
	int i = 0;
	int k = 0;
	vector<vector<_5tuple> > ele(10);
	auto it = pre_flow_table.begin();
	for(; it != pre_flow_table.end(); it++){
		int k = i / 500;
		if(k == 1) break;
		ele[k].push_back(it->first);
		i++;
	}
	i = 0;
	int MOD = 150;
	for (; it != pre_flow_table.end(); it++){
		_5tuple tuplle = it->first;
		if(i % MOD == 0){
			
			MOD = rand() % (500 - 150) + 150;
			int hhn = rand() % (ele[k].size() - (int)(0.9 * ele[k].size())) + (int)(0.9 * ele[k].size());
			//int hhn = ele[k].size();

			vector<int> vc;
			vector<bool> fl(ele[k].size(), false);
			while(vc.size() < ele[k].size()){
				int c = rand() % ele[k].size();
				if(!fl[c]){
					vc.push_back(c);
					fl[c] = true;
				}
			}
			for(int j = 0; j < hhn; j++){
				for(int t = 0; t < 5; t++){
                    _5tuple x = ele[k][vc[j]];
					out.write((char*)&x,sizeof(x));
				}
			}
		}
		i++;
		int c = rand() % ele[k].size();
		int rate = rand() % 100;
		if(rate < 50){
			_5tuple x = ele[k][c];
			out.write((char*)&x,sizeof(x));
		}else{
			out.write((char*)&tuplle,sizeof(_5tuple));
			//out.write((char*)&tuplle,sizeof(_5tuple));
		}
	}
	out.close();
}

int main(int argc, char *argv[] )
{
	int kind[4] = {3, 7};
	//vector<int> cacheList = {100, 200, 300, 400, 500};
	vector<int> cacheList = {100, 200, 400};
	for(int cc = 0; cc < cacheList.size(); cc++) 
		for(int kc = 0; kc < 2; kc++ ) {

		//initial
		int i = kind[kc];
		CACHE_LINE_SIZE = cacheList[cc];
		pre_flow_table.clear();
		flow_table.clear();
		evict_stat.clear();
		cache = vector<vector<cache_node> >(CACHE_LINE_NUM, vector<cache_node>(CACHE_LINE_SIZE));

		HH_Table.clear();
		NF_Table.clear();
		NF_Counter.Csum = NF_Counter.Fsum = 0;

		HH_Hash.clear();
		NF_Hash.clear();
		NF_Hash.resize(2000, 0);


		updateTime = 0;

		srand((unsigned)time(NULL));	// 产生md5 hash中所用伪随机数种子

		fp = fopen(argv[1], "rb");
		/*hy
		string inputFile = "argv[1]";
		ifstream intrace(inputFile.c_str());
		*/

		
		struct _5tuple pre_tuple;
		unsigned int temp1;
		unsigned int temp2;
		//int i = 0;
		// pre-processing

//int	npackets = 0;
//printf("Preprocess...\n");
		while(1)
		{

			if (fscanf(fp, "%d %d %d %d %d %u %u\n",
				&pre_tuple.src_ip, &pre_tuple.dst_ip, &pre_tuple.src_port, &pre_tuple.dst_port, &pre_tuple.proto, &temp1, &temp2) == EOF)
			{
				
				break;
			}

//if ((++npackets & 0xFFFFF) == 0)
//	printf("%d: %d %d %d %d %d %u %u\n", npackets,
//				pre_tuple.src_ip, pre_tuple.dst_ip, pre_tuple.src_port, pre_tuple.dst_port, pre_tuple.proto, temp1, temp2);
/*
			if (fread(&pre_tuple, sizeof(pre_tuple), 1, fp) != 1)	// eof
			{
				break;
			}
*/
			reverse(&pre_tuple);
			if (!pre_flow_table_query(&pre_tuple))
			{
				pre_flow_table_insert(&pre_tuple);
			}
			pre_flow_table_update(&pre_tuple);
		}
printf("flows: %d\n", pre_flow_table.size());
		

		//rebuild();
		//return 0;

		rewind(fp);

		if(i == 1)
		{
			printf("FIFO:\n");
			cache_init(FIFO);
		}
		if(i == 2)
		{
			printf("RANDOM:\n");
			cache_init(RANDOM);
		}
		else if(i == 3)
		{
			printf("LRU:\n");
			cache_init(LRU);
		}
		if(i == 4)
		{
			printf("LFU:\n");
			cache_init(LFU);
		}
		else if(i == 5)
		{
			printf("ADT:\n");
			cache_init(ADT);
		}
		else if(i == 6)
		{
			printf("ADT2:\n");
			cache_init(ADT2);
		}
		else if(i == 7)
		{
			printf("BTE:\n");
			cache_init(BTE);
		}

/*
printf("Replacement...\n");
npackets = 0;
*/
		while(1)
		{
			struct _5tuple tuple;
			struct _5tuple swap_out;
			
			if (fscanf(fp, "%d %d %d %d %d %u %u\n",
				&tuple.src_ip, &tuple.dst_ip, &tuple.src_port, &tuple.dst_port, &tuple.proto, &temp1, &temp2) == Null)
			{
				break;
			}

/*
if (++npackets & 0xFFFFF == 0)
	printf("2#packets: %d\n", npackets);
*/
/*
			if (fread(&tuple, sizeof(tuple), 1, fp) != 1)   // eof
			{
				break;
			}
*/
			reverse(&tuple);

			//x = check(tuple);
			x = check_hash(tuple);

			if (!cache_query(&tuple))	// 如果cache miss
			{
				if (!flow_table_query(&tuple))	// 如果流表也miss，则证明是新流
				{
					flow_table_insert(&tuple);	// 插入流表
				}

				if (cache_swap(&tuple, &swap_out))	// swap_out tuple exists 如果cache行发生替换
				{
					flow_table_update(&swap_out);	// 替换出的元素更新流表
				}
			}

		}

printf("#flows: %d\n", flow_table.size());
		stat();
dump_flow_stats();
		fclose(fp);
	}
	return 0;
}


typedef struct {
	int	num_flows;
	int	evict_times;
	int	hit_times;
} flow_stat_t;

void dump_flow_stats()
{
	flow_stat_t	flow_stats[32], *p;

	for (int i = 0; i < 32; i++) {
		flow_stats[i].num_flows = 0;
		flow_stats[i].evict_times = 0;
		flow_stats[i].hit_times = 0;
	}

	map<struct _5tuple, struct flow_info>::iterator it;
	for (it = flow_table.begin(); it != flow_table.end(); it++) {
		if (it->second.len < 32)
			p = &flow_stats[it->second.len - 1];
		else
			p = &flow_stats[31];
		
		p->num_flows++;
		p->evict_times += it->second.evict_times;
		p->hit_times += it->second.hit_times;
	}

	for (int i = 0; i < 32; i++) {
		if (flow_stats[i].num_flows == 0)
			continue;
		printf("flows[%2d]: #%6d flows, %7d hits, %7d evicts\n", i+1, flow_stats[i].num_flows, flow_stats[i].hit_times, flow_stats[i].evict_times);
	}
	
}
