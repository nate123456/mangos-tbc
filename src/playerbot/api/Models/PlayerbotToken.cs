using System.ComponentModel.DataAnnotations;
using System.ComponentModel.DataAnnotations.Schema;

namespace Playerbot.Api.Models;

[Table("playerbot_tokens")]
public class PlayerbotToken
{
    [Column("accountid")] [Key] public int AccountId { get; set; }
    [Column("token")] public string Token { get; set; }
    [Column("age")] public DateTime Age { get; set; }
}
