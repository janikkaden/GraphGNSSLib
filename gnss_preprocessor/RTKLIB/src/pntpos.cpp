/*------------------------------------------------------------------------------
* pntpos.c : standard positioning
*
*          Copyright (C) 2007-2020 by T.TAKASU, All rights reserved.
*
* version : $Revision:$ $Date:$
* history : 2010/07/28 1.0  moved from rtkcmn.c
*                           changed api:
*                               pntpos()
*                           deleted api:
*                               pntvel()
*           2011/01/12 1.1  add option to include unhealthy satellite
*                           reject duplicated observation data
*                           changed api: ionocorr()
*           2011/11/08 1.2  enable snr mask for single-mode (rtklib_2.4.1_p3)
*           2012/12/25 1.3  add variable snr mask
*           2014/05/26 1.4  support galileo and beidou
*           2015/03/19 1.5  fix bug on ionosphere correction for GLO and BDS
*           2018/10/10 1.6  support api change of satexclude()
*           2020/11/30 1.7  support NavIC/IRNSS in pntpos()
*                           no support IONOOPT_LEX option in ioncorr()
*                           improve handling of TGD correction for each system
*                           use E1-E5b for Galileo dual-freq iono-correction
*                           use API sat2freq() to get carrier frequency
*                           add output of velocity estimation error in estvel()
*-----------------------------------------------------------------------------*/
#include "rtklib.h"

// add by weisong
#include <algorithm>
// google eigen
#include <Eigen/Eigen>
#include <Eigen/Dense>
#include <Eigen/Core>


#include <ros/ros.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>
#include <nav_msgs/Path.h>
#include <nav_msgs/Odometry.h>
#include <geometry_msgs/PointStamped.h>
#include <novatel_oem7_msgs/INSPVAX.h> // novatel_oem7_msgs/INSPVAX
#include <novatel_oem7_msgs/BESTPOS.h> // novatel_oem7_msgs/INSPVAX

#include "../../include/gnss_tools.h"
#include <gnss_msgs/GNSS_Raw_Array.h>
#include <gnss_msgs/GNSS_Raw.h>

FILE* gnss_ublox_wls = fopen("gnss_ublox_wls.csv", "w+");


static const char rcsid[]="$Id:$";

/* constants -----------------------------------------------------------------*/

#define SQR(x)      ((x)*(x))

#if 0 /* enable GPS-QZS time offset estimation */
#define NX          (4+5)       /* # of estimated parameters */
#else
#define NX          (4+4)       /* # of estimated parameters */
#endif
#define MAXITR      10          /* max number of iteration for point pos */
#define ERR_ION     5.0         /* ionospheric delay Std (m) */
#define ERR_TROP    3.0         /* tropspheric delay Std (m) */
#define ERR_SAAS    0.3         /* Saastamoinen model error Std (m) */
#define ERR_BRDCI   0.5         /* broadcast ionosphere model error factor */
#define ERR_CBIAS   0.3         /* code bias error Std (m) */
#define REL_HUMI    0.7         /* relative humidity for Saastamoinen model */
#define MIN_EL      (5.0*D2R)   /* min elevation for measurement error (rad) */

ros::Publisher pub_pntpos_odometry;
ros::Publisher pub_wls_odometry;

ros::Publisher pub_gnss_raw;
ros::Publisher pub_velocity_from_doppler;

GNSS_Tools m_GNSS_Tools; // utilities

extern void pntposRegisterPub(ros::NodeHandle &n)
{
    pub_pntpos_odometry = n.advertise<nav_msgs::Odometry>("WLSENURTKLIB", 1000);
    pub_gnss_raw = n.advertise<gnss_msgs::GNSS_Raw_Array>("GNSSPsrCarRov1", 1000);
    pub_wls_odometry = n.advertise<nav_msgs::Odometry>("WLSENUGoGPS", 1000);
    pub_velocity_from_doppler = n.advertise<nav_msgs::Odometry>("GNSSDopVelRov1", 1000); // velocity_from_doppler
}


/* pseudorange measurement error variance ------------------------------------*/
static double varerr(const prcopt_t *opt, double el, int sys)
{
    double fact,varr;
    fact=sys==SYS_GLO?EFACT_GLO:(sys==SYS_SBS?EFACT_SBS:EFACT_GPS);
    if (el<MIN_EL) el=MIN_EL;
    varr=SQR(opt->err[0])*(SQR(opt->err[1])+SQR(opt->err[2])/sin(el));
    if (opt->ionoopt==IONOOPT_IFLC) varr*=SQR(3.0); /* iono-free */
    return SQR(fact)*varr;
}
/* get group delay parameter (m) ---------------------------------------------*/
static double gettgd(int sat, const nav_t *nav, int type)
{
    int i,sys=satsys(sat,NULL);
    
    if (sys==SYS_GLO) {
        for (i=0;i<nav->ng;i++) {
            if (nav->geph[i].sat==sat) break;
        }
        return (i>=nav->ng)?0.0:-nav->geph[i].dtaun*CLIGHT;
    }
    else {
        for (i=0;i<nav->n;i++) {
            if (nav->eph[i].sat==sat) break;
        }
        return (i>=nav->n)?0.0:nav->eph[i].tgd[type]*CLIGHT;
    }
}
/* test SNR mask -------------------------------------------------------------*/
static int snrmask(const obsd_t *obs, const double *azel, const prcopt_t *opt)
{
    if (testsnr(0,0,azel[1],obs->SNR[0]*SNR_UNIT,&opt->snrmask)) {
        return 0;
    }
    if (opt->ionoopt==IONOOPT_IFLC) {
        if (testsnr(0,1,azel[1],obs->SNR[1]*SNR_UNIT,&opt->snrmask)) return 0;
    }
    return 1;
}
/* psendorange with code bias correction -------------------------------------*/
static double prange(const obsd_t *obs, const nav_t *nav, const prcopt_t *opt,
                     double *var)
{
    double P1,P2,gamma,b1,b2;
    int sat,sys;
    
    sat=obs->sat;
    sys=satsys(sat,NULL);
    P1=obs->P[0];
    P2=obs->P[1];
    *var=0.0;
    
    if (P1==0.0||(opt->ionoopt==IONOOPT_IFLC&&P2==0.0)) return 0.0;
    
    /* P1-C1,P2-C2 DCB correction */
    if (sys==SYS_GPS||sys==SYS_GLO) {
        if (obs->code[0]==CODE_L1C) P1+=nav->cbias[sat-1][1]; /* C1->P1 */
        if (obs->code[1]==CODE_L2C) P2+=nav->cbias[sat-1][2]; /* C2->P2 */
    }
    if (opt->ionoopt==IONOOPT_IFLC) { /* dual-frequency */
        
        if (sys==SYS_GPS||sys==SYS_QZS) { /* L1-L2,G1-G2 */
            gamma=SQR(FREQ1/FREQ2);
            return (P2-gamma*P1)/(1.0-gamma);
        }
        else if (sys==SYS_GLO) { /* G1-G2 */
            gamma=SQR(FREQ1_GLO/FREQ2_GLO);
            return (P2-gamma*P1)/(1.0-gamma);
        }
        else if (sys==SYS_GAL) { /* E1-E5b */
            gamma=SQR(FREQ1/FREQ7);
            if (getseleph(SYS_GAL)) { /* F/NAV */
                P2-=gettgd(sat,nav,0)-gettgd(sat,nav,1); /* BGD_E5aE5b */
            }
            return (P2-gamma*P1)/(1.0-gamma);
        }
        else if (sys==SYS_CMP) { /* B1-B2 */
            gamma=SQR(((obs->code[0]==CODE_L2I)?FREQ1_CMP:FREQ1)/FREQ2_CMP);
            if      (obs->code[0]==CODE_L2I) b1=gettgd(sat,nav,0); /* TGD_B1I */
            else if (obs->code[0]==CODE_L1P) b1=gettgd(sat,nav,2); /* TGD_B1Cp */
            else b1=gettgd(sat,nav,2)+gettgd(sat,nav,4); /* TGD_B1Cp+ISC_B1Cd */
            b2=gettgd(sat,nav,1); /* TGD_B2I/B2bI (m) */
            return ((P2-gamma*P1)-(b2-gamma*b1))/(1.0-gamma);
        }
        else if (sys==SYS_IRN) { /* L5-S */
            gamma=SQR(FREQ5/FREQ9);
            return (P2-gamma*P1)/(1.0-gamma);
        }
    }
    else { /* single-freq (L1/E1/B1) */
        *var=SQR(ERR_CBIAS);
        
        if (sys==SYS_GPS||sys==SYS_QZS) { /* L1 */
            b1=gettgd(sat,nav,0); /* TGD (m) */
            return P1-b1;
        }
        else if (sys==SYS_GLO) { /* G1 */
            gamma=SQR(FREQ1_GLO/FREQ2_GLO);
            b1=gettgd(sat,nav,0); /* -dtaun (m) */
            return P1-b1/(gamma-1.0);
        }
        else if (sys==SYS_GAL) { /* E1 */
            if (getseleph(SYS_GAL)) b1=gettgd(sat,nav,0); /* BGD_E1E5a */
            else                    b1=gettgd(sat,nav,1); /* BGD_E1E5b */
            return P1-b1;
        }
        else if (sys==SYS_CMP) { /* B1I/B1Cp/B1Cd */
            if      (obs->code[0]==CODE_L2I) b1=gettgd(sat,nav,0); /* TGD_B1I */
            else if (obs->code[0]==CODE_L1P) b1=gettgd(sat,nav,2); /* TGD_B1Cp */
            else b1=gettgd(sat,nav,2)+gettgd(sat,nav,4); /* TGD_B1Cp+ISC_B1Cd */
            return P1-b1;
        }
        else if (sys==SYS_IRN) { /* L5 */
            gamma=SQR(FREQ9/FREQ5);
            b1=gettgd(sat,nav,0); /* TGD (m) */
            return P1-gamma*b1;
        }
    }
    return P1;
}
/* ionospheric correction ------------------------------------------------------
* compute ionospheric correction
* args   : gtime_t time     I   time
*          nav_t  *nav      I   navigation data
*          int    sat       I   satellite number
*          double *pos      I   receiver position {lat,lon,h} (rad|m)
*          double *azel     I   azimuth/elevation angle {az,el} (rad)
*          int    ionoopt   I   ionospheric correction option (IONOOPT_???)
*          double *ion      O   ionospheric delay (L1) (m)
*          double *var      O   ionospheric delay (L1) variance (m^2)
* return : status(1:ok,0:error)
*-----------------------------------------------------------------------------*/
extern int ionocorr(gtime_t time, const nav_t *nav, int sat, const double *pos,
                    const double *azel, int ionoopt, double *ion, double *var)
{
    trace(4,"ionocorr: time=%s opt=%d sat=%2d pos=%.3f %.3f azel=%.3f %.3f\n",
          time_str(time,3),ionoopt,sat,pos[0]*R2D,pos[1]*R2D,azel[0]*R2D,
          azel[1]*R2D);
    
    /* GPS broadcast ionosphere model */
    if (ionoopt==IONOOPT_BRDC) {
        *ion=ionmodel(time,nav->ion_gps,pos,azel);
        *var=SQR(*ion*ERR_BRDCI);
        return 1;
    }
    /* SBAS ionosphere model */
    if (ionoopt==IONOOPT_SBAS) {
        return sbsioncorr(time,nav,pos,azel,ion,var);
    }
    /* IONEX TEC model */
    if (ionoopt==IONOOPT_TEC) {
        return iontec(time,nav,pos,azel,1,ion,var);
    }
    /* QZSS broadcast ionosphere model */
    if (ionoopt==IONOOPT_QZS&&norm(nav->ion_qzs,8)>0.0) {
        *ion=ionmodel(time,nav->ion_qzs,pos,azel);
        *var=SQR(*ion*ERR_BRDCI);
        return 1;
    }
    *ion=0.0;
    *var=ionoopt==IONOOPT_OFF?SQR(ERR_ION):0.0;
    return 1;
}
/* tropospheric correction -----------------------------------------------------
* compute tropospheric correction
* args   : gtime_t time     I   time
*          nav_t  *nav      I   navigation data
*          double *pos      I   receiver position {lat,lon,h} (rad|m)
*          double *azel     I   azimuth/elevation angle {az,el} (rad)
*          int    tropopt   I   tropospheric correction option (TROPOPT_???)
*          double *trp      O   tropospheric delay (m)
*          double *var      O   tropospheric delay variance (m^2)
* return : status(1:ok,0:error)
*-----------------------------------------------------------------------------*/
extern int tropcorr(gtime_t time, const nav_t *nav, const double *pos,
                    const double *azel, int tropopt, double *trp, double *var)
{
    trace(4,"tropcorr: time=%s opt=%d pos=%.3f %.3f azel=%.3f %.3f\n",
          time_str(time,3),tropopt,pos[0]*R2D,pos[1]*R2D,azel[0]*R2D,
          azel[1]*R2D);
    
    /* Saastamoinen model */
    if (tropopt==TROPOPT_SAAS||tropopt==TROPOPT_EST||tropopt==TROPOPT_ESTG) {
        *trp=tropmodel(time,pos,azel,REL_HUMI);
        *var=SQR(ERR_SAAS/(sin(azel[1])+0.1));
        return 1;
    }
    /* SBAS (MOPS) troposphere model */
    if (tropopt==TROPOPT_SBAS) {
        *trp=sbstropcorr(time,pos,azel,var);
        return 1;
    }
    /* no correction */
    *trp=0.0;
    *var=tropopt==TROPOPT_OFF?SQR(ERR_TROP):0.0;
    return 1;
}
/* pseudorange residuals -----------------------------------------------------*/
static int rescode(int iter, const obsd_t *obs, int n, const double *rs,
                   const double *dts, const double *vare, const int *svh,
                   const nav_t *nav, const double *x, const prcopt_t *opt,
                   double *v, double *H, double *var, double *azel, int *vsat,
                   double *resp, int *ns)
{
    gtime_t time;
    double r,freq,dion,dtrp,vmeas,vion,vtrp,rr[3],pos[3],dtr,e[3],P;
    int i,j,nv=0,sat,sys,mask[NX-3]={0};
    
    trace(3,"resprng : n=%d\n",n);
    
    for (i=0;i<3;i++) rr[i]=x[i];
    dtr=x[3];
    
    ecef2pos(rr,pos);
    
    for (i=*ns=0;i<n&&i<MAXOBS;i++) {
        vsat[i]=0; azel[i*2]=azel[1+i*2]=resp[i]=0.0;
        time=obs[i].time;
        sat=obs[i].sat;
        if (!(sys=satsys(sat,NULL))) continue;
        
        /* reject duplicated observation data */
        if (i<n-1&&i<MAXOBS-1&&sat==obs[i+1].sat) {
            trace(2,"duplicated obs data %s sat=%d\n",time_str(time,3),sat);
            i++;
            LOG(INFO) <<"duplicated observation data " << time_str(time,3) << " sys = " << satsys(sat,NULL)  << " sat = " << sat;
            continue;
        }
        /* excluded satellite? */
        if (satexclude(sat,vare[i],svh[i],opt)) continue;
        
        /* geometric distance */
        if ((r=geodist(rs+i*6,rr,e))<=0.0) continue;
        
        if (iter>0) {
            /* test elevation mask */
            if (satazel(pos,e,azel+i*2)<opt->elmin) continue;
            
            /* test SNR mask */
            if (!snrmask(obs+i,azel+i*2,opt)) continue;
            
            /* ionospheric correction */
            if (!ionocorr(time,nav,sat,pos,azel+i*2,opt->ionoopt,&dion,&vion)) {
                continue;
            }
            if ((freq=sat2freq(sat,obs[i].code[0],nav))==0.0) continue;
            dion*=SQR(FREQ1/freq);
            vion*=SQR(FREQ1/freq);
            
            /* tropospheric correction */
            if (!tropcorr(time,nav,pos,azel+i*2,opt->tropopt,&dtrp,&vtrp)) {
                continue;
            }
        }
        /* psendorange with code bias correction */
        if ((P=prange(obs+i,nav,opt,&vmeas))==0.0) continue;
        
        /* pseudorange residual */
        v[nv]=P-(r+dtr-CLIGHT*dts[i*2]+dion+dtrp);
        
        /* design matrix */
        for (j=0;j<NX;j++) {
            H[j+nv*NX]=j<3?-e[j]:(j==3?1.0:0.0);
        }
        /* time system offset and receiver bias correction */
        if      (sys==SYS_GLO) {v[nv]-=x[4]; H[4+nv*NX]=1.0; mask[1]=1;}
        else if (sys==SYS_GAL) {v[nv]-=x[5]; H[5+nv*NX]=1.0; mask[2]=1;}
        else if (sys==SYS_CMP) {v[nv]-=x[6]; H[6+nv*NX]=1.0; mask[3]=1;}
        else if (sys==SYS_IRN) {v[nv]-=x[7]; H[7+nv*NX]=1.0; mask[4]=1;}
#if 0 /* enable QZS-GPS time offset estimation */
        else if (sys==SYS_QZS) {v[nv]-=x[8]; H[8+nv*NX]=1.0; mask[5]=1;}
#endif
        else mask[0]=1;
        
        vsat[i]=1; resp[i]=v[nv]; (*ns)++;
        
        /* variance of pseudorange error */
        var[nv++]=varerr(opt,azel[1+i*2],sys)+vare[i]+vmeas+vion+vtrp;
        
        trace(4,"sat=%2d azel=%5.1f %4.1f res=%7.3f sig=%5.3f\n",obs[i].sat,
              azel[i*2]*R2D,azel[1+i*2]*R2D,resp[i],sqrt(var[nv-1]));
    }
    /* constraint to avoid rank-deficient */
    for (i=0;i<NX-3;i++) {
        if (mask[i]) continue;
        v[nv]=0.0;
        for (j=0;j<NX;j++) H[j+nv*NX]=j==i+3?1.0:0.0;
        var[nv++]=0.01;
    }
    return nv;
}
/* validate solution ---------------------------------------------------------*/
static int valsol(const double *azel, const int *vsat, int n,
                  const prcopt_t *opt, const double *v, int nv, int nx,
                  char *msg)
{
    double azels[MAXOBS*2],dop[4],vv;
    int i,ns;
    
    trace(3,"valsol  : n=%d nv=%d\n",n,nv);
    
    /* Chi-square validation of residuals */
    vv=dot(v,v,nv);
    if (nv>nx&&vv>chisqr[nv-nx-1]) {
        sprintf(msg,"chi-square error nv=%d vv=%.1f cs=%.1f",nv,vv,chisqr[nv-nx-1]);
        return 0;
    }
    /* large GDOP check */
    for (i=ns=0;i<n;i++) {
        if (!vsat[i]) continue;
        azels[  ns*2]=azel[  i*2];
        azels[1+ns*2]=azel[1+i*2];
        ns++;
    }
    dops(ns,azels,opt->elmin,dop);
    if (dop[0]<=0.0||dop[0]>opt->maxgdop) {
        sprintf(msg,"gdop error nv=%d gdop=%.1f",nv,dop[0]);
        return 0;
    }
    return 1;
}
/* estimate receiver position ------------------------------------------------*/
static int estpos(const obsd_t *obs, int n, const double *rs, const double *dts,
                  const double *vare, const int *svh, const nav_t *nav,
                  const prcopt_t *opt, sol_t *sol, double *azel, int *vsat,
                  double *resp, char *msg)
{
    double x[NX]={0},dx[NX],Q[NX*NX],*v,*H,*var,sig;
    int i,j,k,info,stat,nv,ns;
    
    trace(3,"estpos  : n=%d\n",n);
    
    v=mat(n+4,1); H=mat(NX,n+4); var=mat(n+4,1);
    
    for (i=0;i<3;i++) x[i]=sol->rr[i];
    
    for (i=0;i<MAXITR;i++) {
        
        /* pseudorange residuals (m) */
        nv=rescode(i,obs,n,rs,dts,vare,svh,nav,x,opt,v,H,var,azel,vsat,resp,
                   &ns);
        
        if (nv<NX) {
            sprintf(msg,"lack of valid sats ns=%d",nv);
            break;
        }
        /* weighted by Std */
        for (j=0;j<nv;j++) {
            sig=sqrt(var[j]);
            v[j]/=sig;
            for (k=0;k<NX;k++) H[k+j*NX]/=sig;
        }
        /* least square estimation */
        if ((info=lsq(H,v,NX,nv,dx,Q))) {
            sprintf(msg,"lsq error info=%d",info);
            break;
        }
        for (j=0;j<NX;j++) {
            x[j]+=dx[j];
        }
        if (norm(dx,NX)<1E-4) {
            sol->type=0;
            sol->time=timeadd(obs[0].time,-x[3]/CLIGHT);
            sol->dtr[0]=x[3]/CLIGHT; /* receiver clock bias (s) */
            sol->dtr[1]=x[4]/CLIGHT; /* GLO-GPS time offset (s) */
            sol->dtr[2]=x[5]/CLIGHT; /* GAL-GPS time offset (s) */
            sol->dtr[3]=x[6]/CLIGHT; /* BDS-GPS time offset (s) */
            sol->dtr[4]=x[7]/CLIGHT; /* IRN-GPS time offset (s) */
            for (j=0;j<6;j++) sol->rr[j]=j<3?x[j]:0.0;
            for (j=0;j<3;j++) sol->qr[j]=(float)Q[j+j*NX];
            sol->qr[3]=(float)Q[1];    /* cov xy */
            sol->qr[4]=(float)Q[2+NX]; /* cov yz */
            sol->qr[5]=(float)Q[2];    /* cov zx */
            sol->ns=(uint8_t)ns;
            sol->age=sol->ratio=0.0;
            
            /* validate solution */
            if ((stat=valsol(azel,vsat,n,opt,v,nv,NX,msg))) {
                sol->stat=opt->sateph==EPHOPT_SBAS?SOLQ_SBAS:SOLQ_SINGLE;
            }
            free(v); free(H); free(var);
            return stat;
        }
    }
    if (i>=MAXITR) sprintf(msg,"iteration divergent i=%d",i);
    
    free(v); free(H); free(var);
    return 0;
}
/* RAIM FDE (failure detection and exclution) -------------------------------*/
static int raim_fde(const obsd_t *obs, int n, const double *rs,
                    const double *dts, const double *vare, const int *svh,
                    const nav_t *nav, const prcopt_t *opt, sol_t *sol,
                    double *azel, int *vsat, double *resp, char *msg)
{
    obsd_t *obs_e;
    sol_t sol_e={{0}};
    char tstr[32],name[16],msg_e[128];
    double *rs_e,*dts_e,*vare_e,*azel_e,*resp_e,rms_e,rms=100.0;
    int i,j,k,nvsat,stat=0,*svh_e,*vsat_e,sat=0;
    
    trace(3,"raim_fde: %s n=%2d\n",time_str(obs[0].time,0),n);
    
    if (!(obs_e=(obsd_t *)malloc(sizeof(obsd_t)*n))) return 0;
    rs_e = mat(6,n); dts_e = mat(2,n); vare_e=mat(1,n); azel_e=zeros(2,n);
    svh_e=imat(1,n); vsat_e=imat(1,n); resp_e=mat(1,n); 
    
    for (i=0;i<n;i++) {
        
        /* satellite exclution */
        for (j=k=0;j<n;j++) {
            if (j==i) continue;
            obs_e[k]=obs[j];
            matcpy(rs_e +6*k,rs +6*j,6,1);
            matcpy(dts_e+2*k,dts+2*j,2,1);
            vare_e[k]=vare[j];
            svh_e[k++]=svh[j];
        }
        /* estimate receiver position without a satellite */
        if (!estpos(obs_e,n-1,rs_e,dts_e,vare_e,svh_e,nav,opt,&sol_e,azel_e,
                    vsat_e,resp_e,msg_e)) {
            trace(3,"raim_fde: exsat=%2d (%s)\n",obs[i].sat,msg);
            continue;
        }
        for (j=nvsat=0,rms_e=0.0;j<n-1;j++) {
            if (!vsat_e[j]) continue;
            rms_e+=SQR(resp_e[j]);
            nvsat++;
        }
        if (nvsat<5) {
            trace(3,"raim_fde: exsat=%2d lack of satellites nvsat=%2d\n",
                  obs[i].sat,nvsat);
            continue;
        }
        rms_e=sqrt(rms_e/nvsat);
        
        trace(3,"raim_fde: exsat=%2d rms=%8.3f\n",obs[i].sat,rms_e);
        
        if (rms_e>rms) continue;
        
        /* save result */
        for (j=k=0;j<n;j++) {
            if (j==i) continue;
            matcpy(azel+2*j,azel_e+2*k,2,1);
            vsat[j]=vsat_e[k];
            resp[j]=resp_e[k++];
        }
        stat=1;
        *sol=sol_e;
        sat=obs[i].sat;
        rms=rms_e;
        vsat[i]=0;
        strcpy(msg,msg_e);
    }
    if (stat) {
        time2str(obs[0].time,tstr,2); satno2id(sat,name);
        trace(2,"%s: %s excluded by raim\n",tstr+11,name);
    }
    free(obs_e);
    free(rs_e ); free(dts_e ); free(vare_e); free(azel_e);
    free(svh_e); free(vsat_e); free(resp_e);
    return stat;
}
/* range rate residuals ------------------------------------------------------*/
static int resdop(const obsd_t *obs, int n, const double *rs, const double *dts,
                  const nav_t *nav, const double *rr, const double *x,
                  const double *azel, const int *vsat, double err, double *v,
                  double *H)
{
    double freq,rate,pos[3],E[9],a[3],e[3],vs[3],cosel,sig;
    int i,j,nv=0;
    
    trace(3,"resdop  : n=%d\n",n);
    
    ecef2pos(rr,pos); xyz2enu(pos,E);
    
    for (i=0;i<n&&i<MAXOBS;i++) {
        
        freq=sat2freq(obs[i].sat,obs[i].code[0],nav);
        
        if (obs[i].D[0]==0.0||freq==0.0||!vsat[i]||norm(rs+3+i*6,3)<=0.0) {
            continue;
        }
        /* LOS (line-of-sight) vector in ECEF */
        cosel=cos(azel[1+i*2]);
        a[0]=sin(azel[i*2])*cosel;
        a[1]=cos(azel[i*2])*cosel;
        a[2]=sin(azel[1+i*2]);
        matmul("TN",3,1,3,1.0,E,a,0.0,e);
        
        /* satellite velocity relative to receiver in ECEF */
        for (j=0;j<3;j++) {
            vs[j]=rs[j+3+i*6]-x[j];
        }
        /* range rate with earth rotation correction */
        rate=dot(vs,e,3)+OMGE/CLIGHT*(rs[4+i*6]*rr[0]+rs[1+i*6]*x[0]-
                                      rs[3+i*6]*rr[1]-rs[  i*6]*x[1]);
        
        /* Std of range rate error (m/s) */
        sig=(err<=0.0)?1.0:err*CLIGHT/freq;
        
        /* range rate residual (m/s) */
        v[nv]=(-obs[i].D[0]*CLIGHT/freq-(rate+x[3]-CLIGHT*dts[1+i*2]))/sig;
        
        /* design matrix */
        for (j=0;j<4;j++) {
            H[j+nv*4]=((j<3)?-e[j]:1.0)/sig;
        }
        nv++;
    }
    return nv;
}
/* estimate receiver velocity ------------------------------------------------*/
static void estvel(const obsd_t *obs, int n, const double *rs, const double *dts,
                   const nav_t *nav, const prcopt_t *opt, sol_t *sol,
                   const double *azel, const int *vsat)
{
    double x[4]={0},dx[4],Q[16],*v,*H;
    double err=opt->err[4]; /* Doppler error (Hz) */
    int i,j,nv;
    
    trace(3,"estvel  : n=%d\n",n);
    
    v=mat(n,1); H=mat(4,n);
    
    for (i=0;i<MAXITR;i++) {
        
        /* range rate residuals (m/s) */
        if ((nv=resdop(obs,n,rs,dts,nav,sol->rr,x,azel,vsat,err,v,H))<4) {
            break;
        }
        /* least square estimation */
        if (lsq(H,v,4,nv,dx,Q)) break;
        
        for (j=0;j<4;j++) x[j]+=dx[j];
        
        if (norm(dx,4)<1E-6) {
            matcpy(sol->rr+3,x,3,1);
            sol->qv[0]=(float)Q[0];  /* xx */
            sol->qv[1]=(float)Q[5];  /* yy */
            sol->qv[2]=(float)Q[10]; /* zz */
            sol->qv[3]=(float)Q[1];  /* xy */
            sol->qv[4]=(float)Q[6];  /* yz */
            sol->qv[5]=(float)Q[2];  /* zx */
            break;
        }
    }
    free(v); free(H);
}
/* single-point positioning ----------------------------------------------------
* compute receiver position, velocity, clock bias by single-point positioning
* with pseudorange and doppler observables
* args   : obsd_t *obs      I   observation data
*          int    n         I   number of observation data
*          nav_t  *nav      I   navigation data
*          prcopt_t *opt    I   processing options
*          sol_t  *sol      IO  solution
*          double *azel     IO  azimuth/elevation angle (rad) (NULL: no output)
*          ssat_t *ssat     IO  satellite status              (NULL: no output)
*          char   *msg      O   error message for error exit
* return : status(1:ok,0:error)
*-----------------------------------------------------------------------------*/
extern int pntpos(const obsd_t *obs, int n, const nav_t *nav,
                  const prcopt_t *opt, sol_t *sol, double *azel, ssat_t *ssat,
                  char *msg)
{
    prcopt_t opt_=*opt;
    double *rs,*dts,*var,*azel_,*resp;
    int i,stat,vsat[MAXOBS]={0},svh[MAXOBS];
    
    trace(3,"pntpos  : tobs=%s n=%d\n",time_str(obs[0].time,3),n);
    
    sol->stat=SOLQ_NONE;
    
    if (n<=0) {
        strcpy(msg,"no observation data");
        return 0;
    }
    sol->time=obs[0].time;
    msg[0]='\0';
    
    rs=mat(6,n); dts=mat(2,n); var=mat(1,n); azel_=zeros(2,n); resp=mat(1,n);
    
    if (opt_.mode!=PMODE_SINGLE) { /* for precise positioning */
        opt_.ionoopt=IONOOPT_BRDC;
        opt_.tropopt=TROPOPT_SAAS;
    }

    /* construct data for WLS with gnss_msgs::GNSS_Raw_Array*/
    gnss_msgs::GNSS_Raw_Array gnss_data;
    int current_week = 0;
    const double current_tow = time2gpst(obs[0].time, &current_week);
    const double gpstime = current_week * 86400 * 7 + current_tow;    // GPS time in seconds from start of GPS epoch inclusive leap seconds (86400 = sec per day)

    /* satellite positons, velocities and clocks */
    satposs(sol->time,obs,n,nav,opt_.sateph,rs,dts,var,svh);
    
    /* estimate receiver position with pseudorange */
    double pos[3];
    stat = estpos(obs,n,rs,dts,var,svh,nav,&opt_,sol,azel_,vsat,resp,msg);
    ecef2pos(sol->rr,pos);
    
    /* limit negative altitude to reduce impact of NLOS */
    pos[2] = std::max(pos[2], -100.0);

    /* estimate receiver position with pseudorange by WLS and Eigen */
    int CMP_cnt = 0, GPS_cnt = 0, GAL_cnt = 0, GLO_cnt = 0, SBS_cnt = 0, QZS_cnt = 0;
    for(int s_i = 0; s_i < n; s_i++)
    {
        /* create GNSS message */
        gnss_msgs::GNSS_Raw gnss_raw;
        
        /* set time */
        gnss_raw.GNSS_time = gpstime;
        
        /* number of satellites */
        gnss_raw.total_sv = int(n);
        
        /* PRN of this satellite */
        gnss_raw.prn_satellites_index = int(obs[s_i].sat);
        
        /* excluded satellite? */
        gnss_raw.valid = (satexclude(obs[s_i].sat,var[s_i],svh[s_i],&opt_) == 0);
        
        /* get snr*/
        gnss_raw.snr = obs[s_i].SNR[0] * SNR_UNIT;

        /* get satellite position */
        gnss_raw.azimuth = azel_[0 + s_i*2] * R2D;
        gnss_raw.elevation = azel_[1 + s_i*2] * R2D;
        gnss_raw.sat_pos_x = rs[0 + s_i * 6];
        gnss_raw.sat_pos_y = rs[1 + s_i * 6];
        gnss_raw.sat_pos_z = rs[2 + s_i * 6];
        
        /* get carrier wave freq */
        double freq;
        if ((freq=sat2freq(obs[s_i].sat,obs[s_i].code[0],nav)) == 0.0)
        {
            continue;
        }
        gnss_raw.lamda = CLIGHT / freq;

        /* pseudorange with code bias correction */
        double P,vmeas;
        if ((P = prange(obs+s_i,nav,&opt_,&vmeas)) == 0.0)
        {
            continue;
        }
        gnss_raw.sat_clk_err = dts[s_i*2] * CLIGHT; // in meter
        
        /* ionospheric correction */
        double dion, vion;
        if (!ionocorr(obs[s_i].time, nav, obs[s_i].sat, pos, azel_+s_i*2, opt->ionoopt, &dion, &vion)) 
        {
            continue;
        }
        dion*=SQR(FREQ1/freq);
        vion*=SQR(FREQ1/freq);
        gnss_raw.err_iono = dion;
        
        /* tropospheric correction */
        double dtrp, vtrp;
        if (!tropcorr(obs[s_i].time, nav, pos, azel_+s_i*2, opt->tropopt, &dtrp, &vtrp))
        {
            continue;
        }
        gnss_raw.err_tropo = dtrp;

        /* get pr*/
        gnss_raw.raw_pseudorange = obs[s_i].P[0];
        
        /* remove the satellite clock bias, atmosphere error here */
        gnss_raw.pseudorange = P + gnss_raw.sat_clk_err - dion - dtrp;
        gnss_raw.carrier_phase = obs[s_i].L[0];

        const int sys = satsys(obs[s_i].sat,NULL);   
        if((gnss_raw.elevation*D2R) > opt_.elmin) // must be '>' to avoid publishing sats with elevation = 0
        {
            if(sys==SYS_GPS)
            {
                GPS_cnt++;
                gnss_raw.sat_system = "GPS";
            }
            else if(sys==SYS_CMP)
            {
                CMP_cnt++;
                gnss_raw.sat_system = "BeiDou";
            }
            else if(sys==SYS_GAL)
            {
                GAL_cnt++;
                gnss_raw.sat_system = "Galileo";
            }
            else if(sys==SYS_GLO)
            {
                GLO_cnt++;
                gnss_raw.sat_system = "GLONASS";
            }
            else if(sys==SYS_SBS)
            {
                SBS_cnt++;
                gnss_raw.sat_system = "SBAS";
            }
            else if(sys==SYS_QZS)
            {
                QZS_cnt++;
                gnss_raw.sat_system = "QZSS";
            }
            else
            {
                LOG(INFO) << "Unknow!!!!! Satellite   "<<current_tow;
            }
            gnss_data.GNSS_Raws.push_back(gnss_raw);
        }
        else
        {
            LOG(INFO) <<"Elevation angle of sat prn nr. " << gnss_raw.prn_satellites_index  
                      << " from sys " << sys << " is " << (gnss_raw.elevation*D2R) 
                      << " <= " << opt_.elmin << " degrees -> ignoring.";
        }
    }

    LOG(INFO) << "GPS_cnt " << "["  << SYS_GPS << "]" << "    " << GPS_cnt;
    LOG(INFO) << "SBS_cnt " << "["  << SYS_SBS << "]" << "    " << SBS_cnt;
    LOG(INFO) << "GLO_cnt " << "["  << SYS_GLO << "]" << "    " << GLO_cnt;
    LOG(INFO) << "GAL_cnt " << "["  << SYS_GAL << "]" << "    " << GAL_cnt;
    LOG(INFO) << "QZS_cnt " << "["  << SYS_QZS << "]" << "   " << QZS_cnt;
    LOG(INFO) << "CMP_cnt " << "["  << SYS_CMP << "]" << "   " << CMP_cnt;
    
    /* publish raw GNSS measurements*/
    pub_gnss_raw.publish(gnss_data);
    
    
    /* TODO: remove eigen dependency in the following code */
    
    #if 1 // PNT from WLS using Eigen
    {
        Eigen::Matrix<double, 3,1> ENU_ref;
        // ENU_ref<< 114.190297420,22.301487386,0;
        ENU_ref<< ref_lon, ref_lat, ref_alt;
        Eigen::Matrix<double, 3, 1> ENU;
        Eigen::MatrixXd eWLSSolutionECEF = m_GNSS_Tools.WeightedLeastSquare(
                                            m_GNSS_Tools.getAllPositions(gnss_data),
                                            m_GNSS_Tools.getAllMeasurements(gnss_data),
                                            gnss_data, "WLS");
        // Eigen::MatrixXd eWLSSolutionECEF = m_GNSS_Tools.WeightedLeastSquare_GPS(
        //                                     m_GNSS_Tools.getAllPositions(gnss_data),
        //                                     m_GNSS_Tools.getAllMeasurements(gnss_data),
        //                                     gnss_data);
        ENU = m_GNSS_Tools.ecef2enu(ENU_ref, eWLSSolutionECEF);
        // std::cout << "eWLSSolutionECEF (wls)-> "<< eWLSSolutionECEF << std::endl;
        // std::cout << "ENU (wls)-> "<< ENU << "    epoch-> "<< current_tow<< std::endl;
        nav_msgs::Odometry odometry;
        odometry.header.frame_id = "map";
        odometry.child_frame_id = "map";
        odometry.pose.pose.position.x = ENU(0);
        odometry.pose.pose.position.y = ENU(1);
        odometry.pose.pose.position.z = 1;
        pub_wls_odometry.publish(odometry);
    }
    #endif
    
    /* RAIM FDE */
    if (!stat&&n>=6&&opt->posopt[4]) {
        stat=raim_fde(obs,n,rs,dts,var,svh,nav,&opt_,sol,azel_,vsat,resp,msg);
    }
    #if 1 /* estimate receiver velocity with Doppler */
    if (stat) {
        estvel(obs,n,rs,dts,nav,&opt_,sol,azel_,vsat);
    }
    else 
    {
        // std::cout<<" the doppler velocity is not estimated due to failure of RAIM"<<std::endl;
        estvel(obs,n,rs,dts,nav,&opt_,sol,azel_,vsat);
    }
    // if (1) estvel(obs,n,rs,dts,nav,&opt_,sol,azel_,vsat, dop_res);
    // std::cout << "norm(dx,n)-> "<<norm(dop_res,n)<<std::endl;
    
    nav_msgs::Odometry odometry;
    odometry.header.frame_id = "map";
    odometry.child_frame_id = "map";
    odometry.pose.pose.position.x = current_tow;
    odometry.twist.twist.linear.x = sol->rr[3];
    odometry.twist.twist.linear.y = sol->rr[4];
    odometry.twist.twist.linear.z = sol->rr[5];
    //odometry.twist.covariance[0] = norm(dop_res,n);

    if(1) // use the snr and ele to model the uncertainty of doppler
    {
        Eigen::MatrixXd covarianceMatrix = m_GNSS_Tools.getCovarianceMatrix(
                                            m_GNSS_Tools.getAllPositions(gnss_data),
                                            m_GNSS_Tools.getAllMeasurements(gnss_data),
                                            gnss_data, "WLS");
        // std::cout << "covarianceMatrix-> \n"<<covarianceMatrix << std::endl;
        odometry.twist.covariance[0] = covarianceMatrix(0,0);
        odometry.twist.covariance[1] = covarianceMatrix(1,1);
        odometry.twist.covariance[2] = covarianceMatrix(2,2);

        // odometry.twist.covariance[0] = sqrt(covarianceMatrix(0,0));
        // odometry.twist.covariance[1] = sqrt(covarianceMatrix(1,1));
        // odometry.twist.covariance[2] = sqrt(covarianceMatrix(2,2));
    }
    
    pub_velocity_from_doppler.publish(odometry);
    #endif
    
    /* TODO: change to sensor_msgs/NavSatFix */
    
    #if 1
    /* Weisong: publish the solution
    *  no matter the solution is good or not
     */
    if(1) // from RTKLIB PNT
    {
        Eigen::Matrix<double, 3,1> ENU_ref;
        // ENU_ref<< 114.190297420,22.301487386,0;
        ENU_ref<< ref_lon, ref_lat, ref_alt;
        Eigen::Matrix<double, 3, 1> ENU;
        Eigen::Matrix<double, 3, 1> ECEF;
        ECEF<<sol->rr[0], sol->rr[1], sol->rr[2];
        ENU = m_GNSS_Tools.ecef2enu(ENU_ref, ECEF);
        // std::cout << "ENU (RTKLIB)-> "<< ENU << std::endl;
        nav_msgs::Odometry odometry;
        odometry.header.frame_id = "map";
        odometry.child_frame_id = "map";
        odometry.pose.pose.position.x = ENU(0);
        odometry.pose.pose.position.y = ENU(1);
        odometry.pose.pose.position.z = 1;
        for(int i = 0; i < 6; i++)
        {
            // odometry.pose.covariance[i] = sol->qr[i];
        }

        odometry.twist.twist.linear.x = sol->rr[3];
        odometry.twist.twist.linear.y = sol->rr[4];
        odometry.twist.twist.linear.z = sol->rr[5];
        pub_pntpos_odometry.publish(odometry);

        if((int(current_tow) > start_gps_sec) && (int(current_tow) < end_gps_sec))
        {
            double pos[3];
            double rr[3]={sol->rr[0], sol->rr[1], sol->rr[2]};
            ecef2pos(sol->rr,pos);
            fprintf(gnss_ublox_wls, "%d,%d,%7.9f,%7.9f,%7.9f,", 2096, int(current_tow),pos[0]*R2D,pos[1]*R2D,pos[2]);
            fprintf(gnss_ublox_wls, "%7.9f,%7.9f,%7.9f \n", sol->qr[0], sol->qr[1],sol->qr[2]);
            fflush(gnss_ublox_wls);
        }
        

    }
    #endif
    
    if (azel) {
        for (i=0;i<n*2;i++) azel[i]=azel_[i];
    }
    if (ssat) {
        for (i=0;i<MAXSAT;i++) {
            ssat[i].vs=0;
            ssat[i].azel[0]=ssat[i].azel[1]=0.0;
            ssat[i].resp[0]=ssat[i].resc[0]=0.0;
            ssat[i].snr[0]=0;
        }
        for (i=0;i<n;i++) {
            ssat[obs[i].sat-1].azel[0]=azel_[  i*2];
            ssat[obs[i].sat-1].azel[1]=azel_[1+i*2];
            ssat[obs[i].sat-1].snr[0]=obs[i].SNR[0];
            if (!vsat[i]) continue;
            ssat[obs[i].sat-1].vs=1;
            ssat[obs[i].sat-1].resp[0]=resp[i];
        }
    }
    free(rs); free(dts); free(var); free(azel_); free(resp);
    return stat;
}
