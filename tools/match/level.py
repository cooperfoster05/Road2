# A check, not a step: how well a game's numbers tell its rank (cross-validated by replay).
import pandas as pd, numpy as np, io
from cols import *
from sklearn.linear_model import RidgeCV
from sklearn.model_selection import GroupKFold, cross_val_predict
L=open('out/stats.csv').read().split('\n')
P=pd.read_csv(io.StringIO('\n'.join(l for l in L if l.startswith('P,'))),names=PCOLS)
P['r']=P['rank'].map({r:i for i,r in enumerate(RANKS)})
F=['avgSpeed','supersonicPct','airPct','stoppedPct','touches','aerialsWon','boostUsed','smallPads','bigPads','saves','shots','fifties','clears','avgBoost','doubleCommits','bunchedPct','leftNetOpen','passes']
per5=['touches','aerialsWon','boostUsed','smallPads','bigPads','saves','shots','fifties','clears','doubleCommits','leftNetOpen','passes']
for c in per5: P[c]=P[c]*300/P['seconds']
for ts in (1,2,3):
    D=P[P.teamSize==ts]; X=D[F].values; y=D['r'].values
    mu,sd=X.mean(0),X.std(0)+1e-9; Z=(X-mu)/sd
    m=RidgeCV(alphas=[1,10,100])
    pr=cross_val_predict(m,Z,y,groups=D['rid'],cv=GroupKFold(5))
    lob=pd.DataFrame({'rid':D['rid'].values,'p':pr,'y':y}).groupby('rid').mean()
    print(ts,'player MAE',np.abs(pr-y).mean().round(2),'within1',(np.abs(pr-y)<=1).mean().round(2),'| lobby MAE',np.abs(lob.p-lob.y).mean().round(2),'within1',(np.abs(lob.p-lob.y)<=1).mean().round(2),'exact',(lob.p.round().clip(0,5)==lob.y).mean().round(2))
    m.fit(Z,y); print('  ',dict(zip(F,m.coef_.round(2))))
