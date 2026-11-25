const storage = new LocalStorage();

class HeroRow {
  name: string = ''; cls: string = ''; initial: string = ''; img: string = ''; classIndex: number = -1; level: number = 0; gold: string = ''; time: string = '';
}

// 暗金风格卡片（数据由 EntryFormAbility.buildCard 给）。2×2：最近一位英雄；2×4：最多三位；4×4：最多六位加「继续游戏」。
// Dark-gold card (data from EntryFormAbility.buildCard). 2×2: the latest hero; 2×4: up to three; 4×4: up to six plus "continue".
@Entry(storage)
@Component
struct WidgetCard {
  @LocalStorageProp('dim') dim: number = 2;
  @LocalStorageProp('has') has: boolean = false;
  @LocalStorageProp('count') count: number = 0;
  @LocalStorageProp('heroes') heroes: HeroRow[] = [];
  @LocalStorageProp('title') title: string = 'Diablo II';
  @LocalStorageProp('empty') empty: string = '';
  @LocalStorageProp('levelWord') levelWord: string = 'Lv';
  @LocalStorageProp('goldWord') goldWord: string = 'Gold';
  @LocalStorageProp('countText') countText: string = '';
  @LocalStorageProp('resume') resume: string = '';

  // Real class artwork when available; missing portraits retain a readable class badge.
  @Builder
  badge(hero: HeroRow, size: number) {
    Stack() {
      if (hero.img) { Image(hero.img).width(size).height(size).objectFit(ImageFit.Contain) }
      else { Text(hero.initial).fontSize(size * 0.46).fontWeight(FontWeight.Bold).fontColor('#F2D38A') }
    }.width(size).height(size).backgroundColor('#1B140D').borderRadius(size / 2)
      .border({ width: 1.5, color: '#9C7A40', radius: size / 2 }).clip(true)
  }

  @Builder
  heroLine(h: HeroRow, big: boolean) {
    Row({ space: 8 }) {
      this.badge(h, big ? 40 : 32)
      Column({ space: 2 }) {
        Text(h.name).fontSize(big ? 15 : 13).fontWeight(FontWeight.Bold).fontColor('#E8C67A').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
        Text(`${this.levelWord === 'Lv' ? 'Lv ' + h.level : h.level + ' ' + this.levelWord} · ${h.cls}`).fontSize(11).fontColor('#D9C9A6').maxLines(1)
      }.alignItems(HorizontalAlign.Start).layoutWeight(1)
      Column({ space: 2 }) {
        Text(`${this.goldWord} ${h.gold}`).fontSize(10).fontColor('#C9A85A').maxLines(1)
        Text(h.time).fontSize(9).fontColor('#7D7466').maxLines(1)
      }.alignItems(HorizontalAlign.End)
    }.width('100%')
  }

  build() {
    Column() {
      if (!this.has) {
        Column({ space: 6 }) {
          Text(this.title).fontSize(11).fontColor('#B08A4A')
          Text('D2R Mobile').fontSize(this.dim === 2 ? 18 : 22).fontWeight(FontWeight.Bold).fontColor('#E8C67A')
          Blank()
          Text(this.empty).fontSize(12).fontColor('#BFAE8A')
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      } else if (this.dim === 2) {
        Column({ space: 6 }) {
          Text(this.title).fontSize(10).fontColor('#8C7345')
          Blank()
          this.badge(this.heroes[0], 44)
          Text(this.heroes[0].name).fontSize(15).fontWeight(FontWeight.Bold).fontColor('#E8C67A').maxLines(1).textOverflow({ overflow: TextOverflow.Ellipsis })
          Text(`${this.levelWord === 'Lv' ? 'Lv ' + this.heroes[0].level : this.heroes[0].level + ' ' + this.levelWord} · ${this.heroes[0].cls}`).fontSize(11).fontColor('#D9C9A6')
          Text(`${this.goldWord} ${this.heroes[0].gold}`).fontSize(10).fontColor('#C9A85A').maxLines(1)
          Text(this.count > 1 ? this.countText : this.heroes[0].time).fontSize(9).fontColor('#7D7466').maxLines(1)
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      } else {
        Column({ space: this.dim === 8 ? 10 : 7 }) {
          Row() {
            Text(this.title).fontSize(11).fontColor('#B08A4A')
            Blank()
            Text(this.countText).fontSize(9).fontColor('#7D7466')
          }.width('100%')
          ForEach(this.heroes, (h: HeroRow) => { this.heroLine(h, this.dim === 8) }, (h: HeroRow) => `${h.name}-${h.level}-${h.gold}`)
          if (this.dim === 8) {
            Blank()
            Text(this.resume).fontSize(14).fontWeight(FontWeight.Bold).fontColor('#1B140D').width('100%').height(36).textAlign(TextAlign.Center)
              .borderRadius(18).linearGradient({ angle: 90, colors: [['#8C6A2C', 0], ['#E8C67A', 1]] })
          }
        }.width('100%').height('100%').alignItems(HorizontalAlign.Start)
      }
    }
    .width('100%').height('100%').padding(12)
    .linearGradient({ angle: 160, colors: [['#2A1E14', 0], ['#0E0B08', 1]] })
    .border({ width: 1, color: '#4A3824', radius: 16 })
    .onClick(() => { postCardAction(this, { action: 'router', abilityName: 'EntryAbility', params: {} }); })
  }
}
