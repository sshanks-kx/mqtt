#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#pragma comment(lib,"ws2_32.lib")
#ifdef MQTT_SSL
#pragma comment(lib,"paho-mqtt3cs.lib")
#else
#pragma comment(lib,"paho-mqtt3c.lib")
#endif
#pragma comment(lib,"q.lib")
#define EXP __declspec(dllexport)
static SOCKET spair[2];
#else
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#define EXP
#define SOCKET_ERROR -1
static int spair[2];
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "socketpair.h"
#include "k.h"

#include <MQTTClient.h>

#ifdef __GNUC__
#define UNUSED(x) x __attribute__((__unused__))
#else
#define UNUSED(x) x
#endif

static MQTTClient client;
static int validinit;

// Function prototypes needed for q callable functions
static void msgsent(void *context, MQTTClient_deliveryToken dt);
static int msgrcvd(void *context, char* topic, int unused, MQTTClient_message *msg);
static void disconn(void *context, char* cause);

static char* getStringFromList(K propValues,int row, const char** value, char* errStr)
{
  if ((int)(kK(propValues)[row]->t) == -KS)
  {
    *value = strdup(kK(propValues)[row]->s);
    return 0;
  }
  return errStr;
}

static char* getCharArrayAsStringFromList(K propValues,int row, const char** value, char* errStr)
{
  if ((int)(kK(propValues)[row]->t) == KC)
  {
    // Don't use strndup because it's not supported on early OS X builds
    char* str = malloc(kK(propValues)[row]->n + 1);
    memcpy(str, kC(kK(propValues)[row]), kK(propValues)[row]->n);
    str[kK(propValues)[row]->n] = '\0';
    *value = str;
    return 0;
  }
  return errStr;
}

static char* getIntFromList(K propValues,int row, int* value, char* errStr)
{
  if ((int)(kK(propValues)[row]->t) == -KI)
    *value = kK(propValues)[row]->i;
  else if ((int)(kK(propValues)[row]->t) == -KJ)
    *value = (int)(kK(propValues)[row]->j);
  else 
    return errStr;
  return 0;
}

static int ssl_error_cb(const char *str, size_t len, void *u) {
  fprintf(stderr,"mqtt ssl error: %.*s\n",(int)len,str);
  return 0;
}

static void freeOpts(MQTTClient_connectOptions* conn_opts){
  free((void*)conn_opts->username);
  free((void*)conn_opts->password);
  free((void*)conn_opts->ssl->trustStore);
  free((void*)conn_opts->ssl->keyStore);
  free((void*)conn_opts->ssl->privateKey);
  free((void*)conn_opts->ssl->privateKeyPassword);
  free((void*)conn_opts->ssl->enabledCipherSuites);
  free((void*)conn_opts->ssl->CApath);
}

#ifdef _WIN32
static void setenv(const char* env,const char* val,int notused){
  int len=strlen(env);
  char* setting=(char*)malloc(len+strlen(val)+2);
  strcpy(setting,env);
  strcpy(setting+len,"=");
  if(val&&*val!=' ')
    strcpy(setting+len+1,val);
  _putenv(setting);
  free(setting);
}
static void unsetenv(const char* env){
  setenv(env,"",1);
}
#endif
static void setProxyEnv(const char* sys,const char* sysval,const char* mqtt){
  if((sysval&&*sysval==0)||(mqtt&&*mqtt==0)) /* unset default env if blank env or blank override env */
    unsetenv(sys);
  if(mqtt&&*mqtt)
    setenv(sys,mqtt,1);                      /* use override env variables value */
}
static void restoreProxyEnv(const char* sys,const char* sysval){
 if(sysval)
   setenv(sys,sysval,1);
}

/* Establish a tcp connection from a q process to mqtt client
 * tcpconn = tcp connection being connected to (symbol)
 * pname   = name to be associated with the connecting process (symbol)
 * opt     = connection options (dict - keys 'username' and 'password' currently supported as symbol types)
*/
EXP K connX(K tcpconn,K pname, K opt){
  static const char* HTTP_PROXY="http_proxy";
  static const char* HTTPS_PROXY="https_proxy";
  const char* system_proxy=getenv(HTTP_PROXY);
  const char* system_proxys=getenv(HTTPS_PROXY);
  int err;
  if(tcpconn->t != -KS)
    return krr("addr type");
  if(pname->t != -KS)
    return krr("client type");
  if(opt->t != XD)
    return krr("options");
  client = 0;

  MQTTClient_willOptions will_opts = MQTTClient_willOptions_initializer;
  MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
  MQTTClient_SSLOptions ssl_opts = MQTTClient_SSLOptions_initializer;

  K propNames = (kK(opt)[0]);
  K propValues = (kK(opt)[1]);
  if(propNames->n > 0)
  {
    if(propNames->t != KS)
      return krr("options key type incorrect");
    if(propValues->t != 0)
      return krr("options value type incorrect");
  }
  int row = 0;
  char* errStr = 0;
  for (;(row<propNames->n && errStr==0);++row)
  {
    if ((kS(propNames)[row])[0] == '\0')
      continue;
    if (strcmp(kS(propNames)[row],"username")==0)
      errStr = getStringFromList(propValues,row,&conn_opts.username,"username type incorrect");
    else if (strcmp(kS(propNames)[row],"password")==0)
      errStr = getStringFromList(propValues,row,&conn_opts.password,"password type incorrect");
    else if (strcmp(kS(propNames)[row],"keepAliveInterval")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.keepAliveInterval,"keepAliveInterval type incorrect");
    else if (strcmp(kS(propNames)[row],"cleansession")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.cleansession,"cleansession type incorrect"); 
    else if (strcmp(kS(propNames)[row],"reliable")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.reliable,"reliable type incorrect");
    else if (strcmp(kS(propNames)[row],"connectTimeout")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.connectTimeout,"connectTimeout type incorrect");
    else if (strcmp(kS(propNames)[row],"retryInterval")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.retryInterval,"retryInterval type incorrect");
    else if (strcmp(kS(propNames)[row],"MQTTVersion")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.MQTTVersion,"MQTTVersion type incorrect");
    else if (strcmp(kS(propNames)[row],"maxInflightMessages")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.maxInflightMessages,"maxInflightMessages type incorrect");
    else if (strcmp(kS(propNames)[row],"cleanstart")==0)
      errStr = getIntFromList(propValues,row,&conn_opts.cleanstart,"cleanstart type incorrect");
    else if (strcmp(kS(propNames)[row],"httpProxy")==0){
      if(conn_opts.struct_version>7)
          errStr = getStringFromList(propValues,row,&conn_opts.httpProxy,"httpProxy type incorrect");
      else
          errStr = "httpProxy requires newer paho mqtt lib";
    }
    else if (strcmp(kS(propNames)[row],"httpsProxy")==0){
      if(conn_opts.struct_version>7)
          errStr = getStringFromList(propValues,row,&conn_opts.httpsProxy,"httpsProxy type incorrect");
      else
          errStr = "httpsProxy requires newer paho mqtt lib";
    }
    else if (strcmp(kS(propNames)[row],"lastWillTopic")==0){
      conn_opts.will = &will_opts;
      errStr = getStringFromList(propValues,row,&will_opts.topicName,"lastWillTopic type incorrect");}
    else if (strcmp(kS(propNames)[row],"lastWillQos")==0)
      errStr = getIntFromList(propValues,row,&will_opts.qos,"lastWillQos type incorrect");
    else if (strcmp(kS(propNames)[row],"lastWillMessage")==0)
      errStr = getCharArrayAsStringFromList(propValues,row,&will_opts.message,"lastWillMessage type incorrect");
    else if (strcmp(kS(propNames)[row],"lastWillRetain")==0)
      errStr = getIntFromList(propValues,row,&will_opts.retained,"lastWillRetain type incorrect");
    else if (strcmp(kS(propNames)[row],"trustStore")==0)
      errStr = getStringFromList(propValues,row,&ssl_opts.trustStore,"trustStore type incorrect");
    else if (strcmp(kS(propNames)[row],"keyStore")==0)
      errStr = getStringFromList(propValues,row,&ssl_opts.keyStore,"keyStore type incorrect");
    else if (strcmp(kS(propNames)[row],"privateKey")==0)
      errStr = getStringFromList(propValues,row,&ssl_opts.privateKey,"privateKey type incorrect");
    else if (strcmp(kS(propNames)[row],"privateKeyPassword")==0)
      errStr = getStringFromList(propValues,row,&ssl_opts.privateKeyPassword,"privateKeyPassword type incorrect");
    else if (strcmp(kS(propNames)[row],"enabledCipherSuites")==0)
      errStr = getStringFromList(propValues,row,&ssl_opts.enabledCipherSuites,"enabledCipherSuites type incorrect");
    else if (strcmp(kS(propNames)[row],"enableServerCertAuth")==0)
      errStr = getIntFromList(propValues,row,&ssl_opts.enableServerCertAuth,"enableServerCertAuth type incorrect");
    else if (strcmp(kS(propNames)[row],"sslVersion")==0)
      errStr = getIntFromList(propValues,row,&ssl_opts.sslVersion,"sslVersion type incorrect");
    else if (strcmp(kS(propNames)[row],"verify")==0)
      errStr = getIntFromList(propValues,row,&ssl_opts.verify,"verify type incorrect");
    else if (strcmp(kS(propNames)[row],"CApath")==0)
      errStr = getStringFromList(propValues,row,&ssl_opts.CApath,"CApath type incorrect");
    else
      errStr = "Unsupported conn opt name in dictionary";
  }

  ssl_opts.ssl_error_cb = *ssl_error_cb;
  conn_opts.ssl = &ssl_opts;

  if(errStr)
    return freeOpts(&conn_opts),krr(errStr);

  if(MQTTCLIENT_SUCCESS != (err = MQTTClient_create(&client, tcpconn->s, pname->s, MQTTCLIENT_PERSISTENCE_NONE, NULL)))
    return freeOpts(&conn_opts),krr((S)MQTTClient_strerror(err));

  MQTTClient_setCallbacks(client, NULL, disconn, msgrcvd, msgsent);

  setProxyEnv(HTTP_PROXY,system_proxy,getenv("mqtt_http_proxy"));     /* replace http_proxy setting with mqtt_http_proxy if available */
  setProxyEnv(HTTPS_PROXY,system_proxys,getenv("mqtt_https_proxy"));  /* replace https_proxy setting with mqtt_https_proxy if available */
  err = MQTTClient_connect(client, &conn_opts);
  restoreProxyEnv(HTTP_PROXY,system_proxy);
  restoreProxyEnv(HTTPS_PROXY,system_proxys);

  freeOpts(&conn_opts);

  if(MQTTCLIENT_SUCCESS != err)
    return krr((S)MQTTClient_strerror(err));
  return (K)0;
}

/* Disconnect from an mqtt client
 * timeout = length of time in ms to allow for the client to clean up prior to disconnection
*/

EXP K disconnect(K timeout){
  if(!MQTTClient_isConnected(client))
    return krr((S)"No client is currently connected");
  else{
    MQTTClient_disconnect(client,(time_t)timeout->i);
    MQTTClient_destroy(&client);
  }
  return (K)0;
}

EXP K isConnected(K UNUSED(x)){
  if(!MQTTClient_isConnected(client))
    return kb(0);
  else
    return kb(1);
}


/* Publish a message to a specified topic
 * topic = topic name as a symbol
 * msg   = message content as a string
 * kqos  = quality of service to be set "ijh"
 * kret  = does the broker retain messages after sending to current subscribers "bijh"
 * For kret/kqos see https://mosquitto.org/man/mqtt-7.html
*/
EXP K pub(K topic, K msg, K kqos, K kret){
  long ret, qos;
  int err;
  if(topic->t != -KS)
    return krr("topic type");
  if(msg->t != KC)
    return krr("payload type");
  switch(kqos->t){
    case -KH:
      qos = kqos->h;
      break;
    case -KI:
      qos = kqos->i;
      break;
    case -KJ:
      qos = kqos->j;
      break;
    default:
      return krr("qos type");
  }
  if(qos<0 || qos>2)
    return krr("invalid qos");
  switch(kret->t){
    case -KB:
      ret = kret->g;
      break;
    case -KH:
      ret = kret->h;
      break;
    case -KI:
      ret = kret->i;
      break;
    case -KJ:
      ret = kret->j;
      break;
    default:
      return krr("retained type");
  }
  if(client == 0)
    return krr("not connected");
  MQTTClient_message pubmsg = MQTTClient_message_initializer;
  pubmsg.payload    = (char*)kG(msg);
  pubmsg.payloadlen = msg->n;
  pubmsg.qos        = qos;
  pubmsg.retained   = ret;
  MQTTClient_deliveryToken token = 0;
  if(MQTTCLIENT_SUCCESS != (err = MQTTClient_publishMessage(client, topic->s, &pubmsg, &token)))
    return krr((S)MQTTClient_strerror(err));
  return kj((J)token);
}

/* Subscribe to a topic
 * topic = topic name as a symbol
*/
EXP K sub(K topic,K kqos){
  int err;
  long qos;
  if(topic->t != -KS)
    return krr("topic type");
  if(client == 0)
    return krr("not connected");
  switch(kqos->t){
    case -KH:
      qos = kqos->h;
      break;
    case -KI:
      qos = kqos->i;
      break;
    case -KJ:
      qos = kqos->j;
      break;
    default:
      return krr("qos type");
  }
  if(MQTTCLIENT_SUCCESS != (err = MQTTClient_subscribe(client, topic->s, qos)))
    return krr((S)MQTTClient_strerror(err));
  return (K)0;
}

/* Unsubscribe from a topic
 * topic = topic name as a symbol
*/
EXP K unsub(K topic){
  int err;
  if(topic->t != -KS)
    return krr("topic type");
  if(client==0)
    return krr("not connected");
  if(MQTTCLIENT_SUCCESS != (err = MQTTClient_unsubscribe(client, topic->s)))
    return krr((S)MQTTClient_strerror(err));
  return (K)0;
}

// Callback data structure
typedef struct CallbackDataStr{
  enum{
      MSG_TYPE_SEND = 9876,   // arbitrary uncommon value
      MSG_TYPE_RCVD,
      MSG_TYPE_DISCONN
    } msg_type;
  unsigned int topic_len;
  union{
    long payload_len;
    MQTTClient_deliveryToken dt;
  } body;
  // Start of dynamic data
} CallbackData;

// Function definitions for above prototypes
static void msgsent(void* context, MQTTClient_deliveryToken dt){
  (void)context;
  // Body contains: <dt>
  CallbackData msg;
  msg.body.dt = dt;
  msg.msg_type = MSG_TYPE_SEND;
  send(spair[1], &msg, sizeof(CallbackData), 0);
}

static char* getSysError(char* buf,int len){
  buf[0]=0;
#ifdef _WIN32
  FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 0,
                WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buf,
                len, 0);
#else
  strerror_r(errno,buf,len);
#endif
  return buf;
}

static int msgrcvd(void* context, char* topic, int unused, MQTTClient_message* mq_msg){
  // Body contains: <topic_len><topic><payload>
  (void)unused;(void)context;
#ifdef _WIN32
  WSABUF buffers[3];
  DWORD bytesSent=0;
#else
  struct iovec iov[3];
#endif
  unsigned int topic_len=strlen(topic);
  CallbackData msg;
  msg.msg_type = MSG_TYPE_RCVD;
  msg.topic_len = topic_len;
  msg.body.payload_len = topic_len + mq_msg->payloadlen;
#ifdef _WIN32
  buffers[0].buf=&msg;
  buffers[0].len=sizeof(CallbackData);
  buffers[1].buf=topic;
  buffers[1].len=topic_len;
  buffers[2].buf=mq_msg->payload;
  buffers[2].len=mq_msg->payloadlen;
  if(SOCKET_ERROR==WSASend(spair[1],buffers,3,&bytesSent,0,NULL,NULL)){
    char buf[256];
    fprintf(stderr, "WSASend error: %s\n", getSysError(buf,sizeof(buf)));
  }
#else
  iov[0].iov_base=&msg;
  iov[0].iov_len=sizeof(CallbackData);
  iov[1].iov_base=topic;
  iov[1].iov_len=topic_len;
  iov[2].iov_base=mq_msg->payload;
  iov[2].iov_len=mq_msg->payloadlen;
  if(-1==writev(spair[1],iov,sizeof(iov)/sizeof(struct iovec))){
    char buf[256];
    fprintf(stderr, "send error: %s\n", getSysError(buf,sizeof(buf)));
  }
#endif
  MQTTClient_freeMessage(&mq_msg);
  MQTTClient_free(topic);
  return 1;
}

static void disconn(void* context, char* cause){
  (void)context;(void)cause;
  // Body contains: <>
  CallbackData msg;
  msg.msg_type = MSG_TYPE_DISCONN;
  send(spair[1], &msg, sizeof(CallbackData), 0);
}

// release K object (print any errors) 
static void pr0(K x){
  if(!x)
  return;
  if(-128 == x->t)
    fprintf(stderr,"%s\n",x->s);
  r0(x);
}

// Callback function definitions
static void qmsgsent(MQTTClient_deliveryToken p){
  pr0(k(0, (char*)".mqtt.msgsent", kj(p), (K)0));
}

static void qdisconn(){
  pr0(k(0, (char*)".mqtt.disconn", ktn(0,0), (K)0));
}

/* Socketpair initialization, callback definition and clean up functionality
 * detach function initialized at exit, socketpair start issues handled
 * callback function set to loop on socketpair connection
*/
K mqttCallback(int fd){
  CallbackData cb_data;
  long rc = recv(fd, (char*)&cb_data, sizeof(cb_data), 0);
  if (rc < (long)sizeof(cb_data)){
    char buf[256];
    fprintf(stderr, "recv(%li) error: %s\n", rc, getSysError(buf,sizeof(buf)));
    return (K)0;
  }
  switch (cb_data.msg_type){
    case MSG_TYPE_SEND:
      qmsgsent(cb_data.body.dt);
      break;
    case MSG_TYPE_RCVD:{
      K topic = ktn(KC,cb_data.topic_len);
      K msg = ktn(KC, cb_data.body.payload_len-cb_data.topic_len);
#ifdef _WIN32
      DWORD actual=0;
      DWORD flags=MSG_WAITALL;
      WSABUF buffers[2];
      buffers[0].buf=&(kG(topic));
      buffers[0].len=cb_data.topic_len;
      buffers[1].buf=&(kG(msg));
      buffers[1].len=msg->n;
      if (WSARecv(fd, buffers, 2, &actual,  &flags, NULL, NULL) == SOCKET_ERROR) {
        char buf[256];
        fprintf(stderr, "WSARecv error: %s\n", getSysError(buf,sizeof(buf)));
      }
#else
      ssize_t actual=0,c=0,iov_c=2;
      struct iovec iov[2];
      iov[0].iov_base=&(kG(topic));
      iov[0].iov_len=cb_data.topic_len;
      iov[1].iov_base=&(kG(msg));
      iov[1].iov_len=msg->n;
      while(actual<cb_data.body.payload_len){
        c=readv(fd,iov,iov_c);
        if(c<=0){
          if(EINTR==errno)
            continue;
          else{
            char buf[256];
            fprintf(stderr, "readv error: %s\n", getSysError(buf,sizeof(buf)));
            break;
          }
        }
        actual+=c;
        if(actual<cb_data.topic_len){
            iov[0].iov_base=&(kG(topic)[actual]);
            iov[0].iov_len=cb_data.topic_len-actual;
        }else{
            iov_c=1;
            iov[0].iov_base=&(kG(msg)[actual-cb_data.topic_len]);
            iov[0].iov_len=msg->n-(actual-cb_data.topic_len);
        }
      }
#endif
      if(actual==cb_data.body.payload_len)
        pr0(k(0, (char*)".mqtt.msgrcvd", topic, msg, (K)0));
      else{
        r0(topic);r0(msg);
      }
      break;
    }
    case MSG_TYPE_DISCONN:
      qdisconn();
      break;
    default:
      fprintf(stderr, "mqttCallback - invalid callback type: %u\n", cb_data.msg_type);
  }
  return (K)0;
}

static void detach(void){
  int sp;
  if((sp=spair[0])){
    sd0x(sp,0);
    close(sp);
  }
  if((sp=spair[1]))
    close(sp);
  spair[0]=0;
  spair[1]=0;
  validinit=0;
}

EXP K init(K UNUSED(X)){
  if(!(0==validinit))
    return 0;
  if(dumb_socketpair(spair,1) == SOCKET_ERROR){
    char buf[256];
    fprintf(stderr,"Init failed. socketpair: %s\n", getSysError(buf,sizeof(buf)));
    return 0;
  }
  pr0(sd1(spair[0], &mqttCallback));
  validinit = 1;
  atexit(detach);
  return 0;
}

