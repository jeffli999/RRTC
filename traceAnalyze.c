#include <stdio.h>
#include <stdlib.h>
#include <pcap.h>
#include <string.h>
#include <map>
#include <iostream>

using namespace std;

//五元组,protocal:1B,src_ip/dst_ip:4B,src_port/dst_port:2B,-- total:13B
typedef struct {
    u_char packet[13];
}tuples;
//结构体，用于统计数据包落在某一范围内的流的信息
typedef struct{
    int flow_sum;
    int packet_sum;
}statics;

int num = 0;//统计数据包数目
int s_num = 0,No;//统计特定数据包的数目
int udp_num = 0;
int tcp_num = 0;
FILE *wfp;
FILE *UDPfp;
FILE *TCPfp;
tuples p; //数据包的五元组
int s_packet[13];//特定数据包

typedef struct pcap_pkthdr PCAP_PKTHEADER;
int findTuples(PCAP_PKTHEADER *pkt_Header,const u_char *pktData);

//索引的比较函数
struct ptrCmp  
{  
   bool operator()(const tuples &a ,const tuples &b)
	{
	    int i;
	    for(i = 0; i < 13; i++)
	    {
		if(a.packet[i]-b.packet[i] > 0)
			return true;
		else if(a.packet[i]-b.packet[i] < 0)
			return false;
	    }
	    return false;
	}
};  

//流 用map实现
map<tuples,int,ptrCmp> flow;
map<tuples,int,ptrCmp>::iterator iter;
std::pair< std::map<tuples,int,ptrCmp>::iterator,bool > ret;

//找数据包的五元组
int findTuples(PCAP_PKTHEADER *pktHeader,const u_char *pktData);

//流信息打印函数
void print(map<tuples,int,ptrCmp> flow);
//跟踪某个特定流
int follow_onefl();
int follow_UDPfl();
int follow_TCPfl();

int main(int argc, char *argv[])
{//参数为要统计的.pcap文件
    int i,j,flag;
    wfp = fopen("special.ots", "w");
    UDPfp = fopen("udp.ots", "w");
    TCPfp = fopen("tcp.ots", "w");
    if (!wfp)
    {
        perror("cannot open file");
        //exit(-1);
    }
    if(argc<2)
    {
        fprintf(stdout,"please input test filename\n");
        return 0;
    }
    if(argc>2)
    {
    	for(i = 0; i < 13; i++)
	    s_packet[i]= atoi(argv[i+2]);
    }
    fprintf (stdout, "test filename=%s\n", argv[1]);
    /*for(i = 0; i < 13; i++)
    {
	scanf("%d ",&s_packet[i]);
    }*/
    for(i = 0; i < 13; i++)
    {
	printf("%d ", s_packet[i]);
    }
    printf("\n");
   
    //打开指定的数据包文件
    char *dev, errBuff[PCAP_ERRBUF_SIZE];
    pcap_t *handle = NULL;
 
    handle = pcap_open_offline( argv[1] , errBuff);
 
    if (NULL == handle) 
    {
        fprintf(stderr, "Error: %s\n", errBuff);
        return (EXIT_FAILURE);
    }
   
    #if defined(_DEBUG_)
        fprintf(stdout,"running pcap_next\n");
    #endif
   
       
    //读取数据包文件，并对每一数据包进行处理
    PCAP_PKTHEADER *pktHeader;
    int status;
    const u_char *pktData;
   
    do 
    {
        #if defined(_DEBUG_)
           fprintf(stdout, "status: %d\n", status);
        #endif
        status = pcap_next_ex(handle, &pktHeader, &pktData );//每次往下读取一条流信息，用&pktHeader和&pktData接收
	// status = pcap_next(handle, &pktHeader );
	
	//提取五元组	
	flag = findTuples(pktHeader, pktData);
	num++;
	if(!flag)    continue;//只统计UDP或TCP的数据包
	/*跟踪某个特定流*/
	follow_onefl();
	/*跟踪UDP流*/
	if(p.packet[0] == 17) follow_UDPfl();
	/*跟踪TCP流*/
	if(p.packet[0] == 6)  follow_TCPfl();
	//流信息统计
	ret = flow.insert(pair<tuples,int>(p, 1));
	if(! ret.second)
		++ret.first->second;
	
 
    } while (status == 1);
    printf("total_packet_num : %d\n",num);
    //打印流信息
    print(flow);
    fclose(wfp);
    fclose(UDPfp);
    fclose(TCPfp);
    pcap_close(handle);
   
    return (EXIT_SUCCESS);
}

static int dis2 = 0;
static int dis5 = 0;
static int dis20 = 0;
static int dis50 = 0;
static int dis100 = 0;
static int dis200 = 0;
static int dis500 = 0;
static int dis1000 = 0;
static int dis1001 = 0;
map<int, int> mdis;
int follow_onefl()
{
    int i,t;
    for(i = 0; i < 13; i++)
    {
	if(p.packet[i] != (int)s_packet[i])
	    return 0;
    }
	No++;
	t = num-s_num;
	s_num = num;
	if(t < 2) dis2++;
	else if(t < 5) dis5++;
        else if(t < 20) dis20++;
        else if(t < 50) dis50++;
        else if(t < 100) dis100++;
	else if(t < 200) dis200++;
	else if(t < 500) dis500++;
        else if(t < 1000) dis1000++;
        else dis1001++;
	//fprintf(wfp,"%d ",num);
	if(No == 1) return 0;
	//fprintf(wfp,"%d",t);
	//fputc('\n', wfp);
	mdis[t]++;
    return 1;
}

int follow_UDPfl()
{
    int t;
    t = num - udp_num;
    udp_num = num;
    fprintf(UDPfp,"%d",t);
    fputc('\n',UDPfp);
    return 1;
}
int follow_TCPfl()
{
    int t;
    t = num - tcp_num;
    tcp_num = num;
    fprintf(TCPfp,"%d",t);
    fputc('\n',TCPfp);
    return 1;
}
int findTuples(PCAP_PKTHEADER *pktHeader,const u_char *pktData)
{
    #if defined(_DEBUG_)      
	 fprintf(stdout,"running printPktHeader\n");
    #endif
    const u_char *ip_h = pktData;//要统计的数据包没有link layer
    p.packet[0] = ip_h[9];//protocal
    if(!(p.packet[0] == 6  || p.packet[0] == 17))
	return 0;
    for(int i = 1; i <= 12; i++)
    {
	p.packet[i]=ip_h[12+i-1];//src_ip，dst_ip, src_port，dst_port
    }
    return 1;
}
void print(map<tuples,int,ptrCmp> flow)
{
    statics s[10];
    int mins = 0;
    tuples tmins;
    for(int i = 1; i < 10; i++)
    {
	s[i].flow_sum = 0;
	s[i].packet_sum = 0;
    }
    iter = flow.begin();
    for(;iter!=flow.end();++iter)
    {
        if(iter->second > mins){
            mins = iter->second;
            tmins = iter->first;
	}
	if(iter ->second ==1)
	{
		s[1].flow_sum++;
		s[1].packet_sum += iter ->second;
	}
	else if(iter ->second >= 2 && iter ->second <= 5)
	{
		s[2].flow_sum++;
		s[2].packet_sum += iter ->second;
	}
	else if(iter ->second >= 6 && iter ->second <= 20)
	{
		s[3].flow_sum++;
		s[3].packet_sum += iter ->second;
	}
	else if(iter ->second >= 21 && iter ->second <= 50)
	{
		s[4].flow_sum++;
		s[4].packet_sum += iter ->second;
	}
	else if(iter ->second >= 51 && iter ->second <= 100)
	{
		s[5].flow_sum++;
		s[5].packet_sum += iter ->second;
	}
	else if(iter ->second >= 101 && iter ->second <= 200)
	{
		s[6].flow_sum++;
		s[6].packet_sum += iter ->second;
	}
	else if(iter ->second >= 201 && iter ->second <= 500)
	{
		s[7].flow_sum++;
		s[7].packet_sum += iter ->second;
	}
	else if(iter ->second >= 501 && iter ->second <= 1000)
	{
		s[8].flow_sum++;
		s[8].packet_sum += iter ->second;
	}
	else if(iter ->second >= 1001)
	{
		s[9].flow_sum++;
		s[9].packet_sum += iter ->second;
	}
    }
    printf("packet_range	flow_sum	packet_sum\n");
    printf("           1	%d		%d	\n",s[1].flow_sum,s[1].packet_sum);
    printf("       [2,5]	%d		%d	\n",s[2].flow_sum,s[2].packet_sum);
    printf("      [6,20]	%d		%d	\n",s[3].flow_sum,s[3].packet_sum);
    printf("     [21,50]	%d		%d	\n",s[4].flow_sum,s[4].packet_sum);
    printf("    [51,100]	%d		%d	\n",s[5].flow_sum,s[5].packet_sum);
    printf("   [101,200]	%d		%d	\n",s[6].flow_sum,s[6].packet_sum);
    printf("   [201,500]	%d		%d	\n",s[7].flow_sum,s[7].packet_sum);
    printf("  [501,1000]	%d		%d	\n",s[8].flow_sum,s[8].packet_sum);
    printf("     [1001,]	%d		%d	\n",s[9].flow_sum,s[9].packet_sum);

    printf("max tuple and its packet number %d\n", mins);
    for(int i = 0; i < 13; i++)
    {
        printf("%d ", tmins.packet[i]);
    }
    printf("\n");
    printf("dist_range         sum	\n");
    printf("       [0,2)        %d      \n", dis2);
    printf("       [2,5)        %d      \n", dis5);
    printf("      [5,20)        %d      \n", dis20);
    printf("     [20,50)        %d      \n", dis50);
    printf("    [50,100)        %d      \n", dis100);
    printf("   [100,200)        %d      \n", dis200);
    printf("   [200,500)        %d      \n", dis500);
    printf("  [500,1000)        %d      \n", dis1000);
    printf("    [1000,~)        %d      \n", dis1001);

    int k = (mdis.rbegin())->first;
    for(int i = 1; i <= k; i++){
	if(mdis.count(i)){
		fprintf(wfp,"%d",mdis[i]);
	}else fprintf(wfp,"%d",0); 
	fputc('\n', wfp);

    }


}
