# A check, not a step: which positioning numbers tell a game's rank, and how well (cross-validated by replay).
import pandas as pd, numpy as np, io
from cols import *
from sklearn.linear_model import RidgeCV
from sklearn.model_selection import GroupKFold, cross_val_predict
L=open('out/stats.csv').read().split('\n')
P=pd.read_csv(io.StringIO('\n'.join(l for l in L if l.startswith('P,'))),names=PCOLS)
P['r']=P['rank'].map({r:i for i,r in enumerate(RANKS)})
for c in ['beaten','leftNetOpen','shadows','doubleCommits','cutRotations','leftForBoost']: P[c]=P[c]*300/P['seconds']
S1=['goalSideTheirsPct','farFromPlayPct','behindBallPct','beaten','leftNetOpen','recoverSecs','shadows','stoppedPct']
S2=S1+['doubleCommits','bunchedPct','flatPct','supportBack','supportDist','lastDefDepth','lastAttDepth','cutRotations','leftForBoost','wideSupportPct']
for ts,F in ((1,S1),(2,S2),(3,S2)):
    D=P[P.teamSize==ts]; X=D[F].values; y=D['r'].values
    mu,sd=X.mean(0),X.std(0)+1e-9; Z=(X-mu)/sd
    pr=cross_val_predict(RidgeCV(alphas=[1,10,100]),Z,y,groups=D['rid'],cv=GroupKFold(5))
    lob=pd.DataFrame({'rid':D['rid'].values,'p':pr,'y':y}).groupby('rid').mean()
    print(ts,'player corr %.2f MAE %.2f within1 %.2f | lobby corr %.2f within1 %.2f'%(np.corrcoef(pr,y)[0,1],np.abs(pr-y).mean(),(np.abs(pr-y)<=1).mean(),np.corrcoef(lob.p,lob.y)[0,1],(np.abs(lob.p-lob.y)<=1).mean()))
    m=RidgeCV(alphas=[1,10,100]).fit(Z,y); print('   ',', '.join(f'{f} {w:+.2f}' for f,w in sorted(zip(F,m.coef_),key=lambda x:-abs(x[1]))))
